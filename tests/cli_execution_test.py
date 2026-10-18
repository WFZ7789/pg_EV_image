import argparse
import hashlib
import json
import math
import struct
from pathlib import Path
import subprocess
import tempfile
import zlib


def run(command, expected=0):
    result = subprocess.run([str(x) for x in command], capture_output=True, text=True, timeout=60)
    if result.returncode != expected:
        raise RuntimeError(f"Command failed ({result.returncode}): {command}\n{result.stdout}\n{result.stderr}")
    return result


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def png_pixels(path, expected_depth=8):
    data = path.read_bytes()
    check(data[:8] == b"\x89PNG\r\n\x1a\n", "Invalid PNG signature")
    offset, compressed = 8, bytearray()
    while offset < len(data):
        size = struct.unpack(">I", data[offset:offset + 4])[0]
        kind = data[offset + 4:offset + 8]
        chunk = data[offset + 8:offset + 8 + size]
        if kind == b"IHDR":
            width, height, depth, color, compression, filtering, interlace = struct.unpack(">IIBBBBB", chunk)
            check((depth, color, compression, filtering, interlace) == (expected_depth, 2, 0, 0, 0), "Unexpected PNG representation")
        if kind == b"IDAT":
            compressed.extend(chunk)
        offset += size + 12
    raw = zlib.decompress(compressed)
    bytes_per_pixel = 3 * (depth // 8)
    stride, pixels, previous = width * bytes_per_pixel, bytearray(), bytearray(width * bytes_per_pixel)
    check(len(raw) == height * (stride + 1), "Invalid PNG data size")
    for row in range(height):
        start = row * (stride + 1)
        filtering, decoded = raw[start], bytearray(raw[start + 1:start + 1 + stride])
        check(filtering <= 4, "Unknown PNG filter")
        for i in range(stride):
            left, up, corner = decoded[i - bytes_per_pixel] if i >= bytes_per_pixel else 0, previous[i], previous[i - bytes_per_pixel] if i >= bytes_per_pixel else 0
            if filtering == 0:
                predictor = 0
            elif filtering == 1:
                predictor = left
            elif filtering == 2:
                predictor = up
            elif filtering == 3:
                predictor = (left + up) // 2
            else:
                estimate = left + up - corner
                distances = (abs(estimate - left), abs(estimate - up), abs(estimate - corner))
                predictor = (left, up, corner)[distances.index(min(distances))]
            decoded[i] = (decoded[i] + predictor) & 255
        pixels.extend(decoded)
        previous = decoded
    return width, height, pixels


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--baseline", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="pg-ev-cli-test-") as temporary:
        directory = Path(temporary)
        run([args.fixture, directory])
        sources = sorted(directory.glob("*.HDF"))
        scratch = directory / "scratch"
        scratch.mkdir()
        sentinel = scratch / "keep.txt"
        sentinel.write_text("keep")
        reference_hashes = {}
        for command in ("render", "mosaic"):
            for threads in (1, 4, 0):
                output = directory / f"{command}-{threads}.png"
                if command == "render":
                    arguments = ["render", "-m", "E", "-b", "24", "-d", sources[0], "-g", sources[0],
                                 "--width", "256", "-o", output]
                else:
                    job = {"version": 1, "mode": "E", "band": 24,
                           "inputs": [{"data": str(path), "geo": str(path)} for path in sources],
                           "map": {"width": 256}, "output": str(output)}
                    job_path = directory / "job.json"
                    job_path.write_text(json.dumps(job))
                    arguments = ["mosaic", "--job", job_path]
                run([args.app, *arguments, "--threads", threads, "--profile", "--scratch-dir", scratch])
                profile = json.loads(Path(str(output) + ".profile.json").read_text())
                check(profile["success"] is True and profile["scope"] == ("native_swath" if command == "render" else "native_mosaic"),
                      "Wrong profile status")
                check(profile["requested_threads"] == threads and profile["thread_limit"] >= 1, "Wrong thread count")
                if threads:
                    check(profile["thread_limit"] == threads, "Explicit thread limit was lost")
                check(profile["wall_seconds"] > 0 and profile["phases"]["native_render"]["calls"] == 1,
                      "Missing timing")
                for field in ("io", "computed_samples", "source_pixels", "output", "buffers", "tracked_buffer_peak_bytes"):
                    check(profile[field] is not None, "Missing native measurement: " + field)
                dimensions = struct.unpack(">II", output.read_bytes()[16:24])
                check(dimensions == (profile["output"]["width"], profile["output"]["height"]), "Wrong output dimensions")
                check(dimensions[0] == 276, "Requested body width or scaled margin was lost")
                count = 1 if command == "render" else len(sources)
                check(profile["phases"]["product"]["calls"] == count and profile["phases"]["read"]["calls"] == count,
                      "Scene product or source samples were generated more than once")
                check(profile["source_pixels"] == 32 * 64 * count and profile["computed_samples"] == 2 * 32 * 64 * count,
                      "Unexpected calibration or product work count")
                for usage in profile["buffers"].values():
                    check(usage["current_bytes"] == 0, "Native render retained a buffer")
                if command == "mosaic":
                    cache = profile["cache"]
                    check(cache["scenes"] == count and cache["read_bytes"] > 0 and cache["write_bytes"] > 0,
                          "Mosaic did not reuse its scene cache")
                    check(cache["current_bytes"] == 0 and cache["peak_bytes"] > 0, "Cache files were not released")
                else:
                    check(profile["cache"] is None, "Single scene unexpectedly used a disk cache")
                check(profile["scratch_bytes"] == 0, "Task cache was not cleaned")
                check(sorted(scratch.iterdir()) == [sentinel], "Task cleanup removed unrelated files or leaked a cache")
                current_hash = digest(output)
                if command in reference_hashes:
                    check(current_hash == reference_hashes[command], "Thread count changed image output")
                reference_hashes[command] = current_hash
                if args.baseline and threads == 1:
                    legacy_output = directory / f"legacy-{command}.png"
                    if command == "render":
                        legacy_arguments = list(arguments)
                        legacy_arguments[legacy_arguments.index("-o") + 1] = legacy_output
                    else:
                        legacy_job = dict(job, output=str(legacy_output))
                        legacy_path = directory / "legacy-job.json"
                        legacy_path.write_text(json.dumps(legacy_job))
                        legacy_arguments = ["mosaic", "--job", legacy_path]
                    run([args.baseline, *legacy_arguments])
                    check(struct.unpack(">I", legacy_output.read_bytes()[16:20])[0] == dimensions[0],
                          "Legacy smoke render has a different body-width layout")
            print(command, "outputs match for 1, 4 and auto threads", flush=True)
        reverse_output = directory / "reverse.png"
        reverse_job = dict(job, inputs=list(reversed(job["inputs"])), output=str(reverse_output))
        reverse_path = directory / "reverse-job.json"
        reverse_path.write_text(json.dumps(reverse_job))
        run([args.app, "mosaic", "--job", reverse_path, "--threads", "4", "--profile", "--scratch-dir", scratch])
        check(digest(reverse_output) == reference_hashes["mosaic"], "Input order changed time/path overlap priority")
        def f32(value):
            return struct.unpack("f", struct.pack("f", value))[0]

        for mode, channel in (("RT", 0), ("RY", 2), ("RMUL", 2)):
            values = []
            factor = f32(1 / f32(math.cos(math.radians(30))))
            for shift in (0, 3):
                for row in range(32):
                    for col in range(64):
                        raw = 10 + (row * 3 + col * 5 + channel * 7) % 80 + shift
                        values.append(f32(f32(raw / 100) * factor))
            quantile = sorted(values)[math.ceil(len(values) * .99) - 1]
            expected_scale = f32(math.floor(quantile * 65535 / 8) * 8 / 65535)
            hashes = []
            for threads in (1, 4):
                mode_output = directory / f"{mode}-{threads}.png"
                mode_path = directory / f"{mode}-{threads}.json"
                mode_path.write_text(json.dumps(dict(job, mode=mode, band=3, output=str(mode_output))))
                run([args.app, "mosaic", "--job", mode_path, "--threads", threads, "--profile", "--scratch-dir", scratch])
                profile = json.loads(Path(str(mode_output) + ".profile.json").read_text())
                check(profile["shared_display_scale"] == [0, expected_scale], "Shared histogram rank or domain changed")
                check(profile["phases"]["product"]["calls"] == 2 and profile["phases"]["read"]["calls"] == (4 if mode == "RMUL" else 8),
                      "Mosaic repeated its product calculation or source reads")
                hashes.append(digest(mode_output))
            check(hashes[0] == hashes[1], "Thread count changed shared-stretch output")
        saturated = directory / "saturated"
        run([args.fixture, saturated, "saturated"])
        saturated_output = directory / "saturated.png"
        saturated_path = directory / "saturated.json"
        saturated_path.write_text(json.dumps(dict(job, mode="RT", band=3, output=str(saturated_output),
            inputs=[{"data": str(path), "geo": str(path)} for path in sorted(saturated.glob("*.HDF"))])))
        run([args.app, "mosaic", "--job", saturated_path, "--profile", "--scratch-dir", scratch], expected=2)
        failed = json.loads(Path(str(saturated_output) + ".profile.json").read_text())
        check(failed["success"] is False and failed["cache"]["scenes"] == 2 and failed["cache"]["current_bytes"] == 0,
              "Statistics failure after scene preparation did not release its cache")
        check(not saturated_output.exists() and sorted(scratch.iterdir()) == [sentinel], "Failed shared stretch leaked data")
        for name, inputs in (("duplicate", [job["inputs"][0]] * 2),
                             ("missing-geo", [{"data": item["data"]} for item in job["inputs"]])):
            failed_output = directory / f"{name}.png"
            failed_path = directory / f"{name}.json"
            failed_path.write_text(json.dumps(dict(job, inputs=inputs, output=str(failed_output))))
            run([args.app, "mosaic", "--job", failed_path, "--profile", "--scratch-dir", scratch], expected=2)
            profile = json.loads(Path(str(failed_output) + ".profile.json").read_text())
            check(profile["success"] is False and profile["scope"] == "native_mosaic", "Wrong failed mosaic status")
            check(not failed_output.exists() and sorted(scratch.iterdir()) == [sentinel], "Failed mosaic leaked output or cache")
        grid_output = directory / "grid.png"
        run([args.app, "render", "-m", "E", "-b", "24", "-d", sources[0], "-g", sources[0],
             "--width", "256", "--grid", "--grid-step", "2", "-o", grid_output, "--profile"])
        width, height, plain = png_pixels(directory / "render-1.png")
        grid_width, grid_height, overlaid = png_pixels(grid_output)
        check((width, height) == (grid_width, grid_height), "Grid changed the layout")
        changed = False
        for row in range(height):
            for col in range(width):
                i = (row * width + col) * 3
                difference = plain[i:i + 3] != overlaid[i:i + 3]
                if row < 11 or row >= height - 11 or col >= 256:
                    check(not difference, "Grid modified annotations or margins")
                changed |= difference
        check(changed, "Grid did not change any map pixels")
        profile = json.loads(Path(str(grid_output) + ".profile.json").read_text())
        check(profile["buffers"]["framebuffer"]["peak_bytes"] == (width * height + 256 * (height - 22)) * 3,
              "Grid layer was omitted from buffer accounting")
        ortho = directory / "ortho.png"
        run([args.app, "render", "-m", "E", "-b", "24", "-d", sources[0], "-g", sources[0],
             "--width", "256", "--projection", "orthographic", "--center-lat", "13", "--center-lon", "146",
             "--half-range", "2", "--grid", "-o", ortho, "--profile"])
        profile = json.loads(Path(str(ortho) + ".profile.json").read_text())
        check(profile["scope"] == "native_swath" and profile["success"], "Orthographic crop failed")
        check(profile["source_pixels"] == 32 * 64, "Crop changed full-scene product statistics")
        print("Native final layout, grid isolation and orthographic crop passed", flush=True)
        output = directory / "failed.png"
        run([args.app, "render", "-m", "E", "-b", "999", "-d", sources[0], "-g", sources[0],
             "-o", output, "--profile", "--scratch-dir", scratch], expected=2)
        profile = json.loads(Path(str(output) + ".profile.json").read_text())
        check(profile["success"] is False, "Failed render was recorded as successful")
        check(sorted(scratch.iterdir()) == [sentinel], "Failure leaked scratch files")
        result = subprocess.run([str(args.app), "render", "-m", "E", "-d", str(sources[0]), "--threads", "-1"],
                                capture_output=True, timeout=60)
        check(result.returncode != 0, "Negative thread count was accepted")
        print("CLI profiles, failure handling and scratch cleanup passed", flush=True)


if __name__ == "__main__":
    main()
