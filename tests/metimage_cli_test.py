import argparse
import hashlib
import json
from pathlib import Path
import tempfile
import zipfile

from cli_execution_test import check, png_pixels, run


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="pg-ev-metimage-") as temporary:
        directory = Path(temporary)
        first, second, night = [directory / name for name in ("first.nc", "second.nc", "night.nc")]
        run([args.fixture, "--fixture", first])
        run([args.fixture, "--fixture", second, "day", "1"])
        run([args.fixture, "--fixture", night, "night"])
        archive = directory / "first.zip"
        with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as package:
            package.write(first, "first.nc")
            package.writestr("manifest.xml", "<manifest/>")
        for path in (first, archive):
            summary = run([args.app, "inspect", path]).stdout
            check("Metop-SG A1" in summary and "Band 19: 13345 nm" in summary, "METimage inspect")
        scratch = directory / "scratch"
        scratch.mkdir()
        base = [args.app, "render", "--width", "384", "--threads", "4", "--scratch-dir", scratch]
        outputs = []
        for path in (first, archive):
            output = directory / (path.suffix[1:] + ".png")
            run([*base, "-d", path, "-m", "RT", "-o", output])
            outputs.append(output.read_bytes())
        check(outputs[0] == outputs[1], "ZIP/NC image equivalence")
        for band in range(20):
            output = directory / f"band-{band}.png"
            run([*base, "-d", first, "-m", "R" if band < 11 else "E", "-b", str(band), "-o", output])
            width, height, pixels = png_pixels(output)
            check(width == 414 and height > 30 and any(pixels), "METimage single-channel output")
        for mode in ("RMUL", "RY", "RF", "EC", "ECT", "TSG"):
            run([*base, "-d", archive, "-m", mode, "-o", directory / f"single-{mode}.png"])
        for mode in ("E", "EC", "ECT"):
            run([*base, "-d", night, "-m", mode, "-b", "17", "-o", directory / f"night-{mode}.png"])
        for extension in ("png", "jpg", "tiff"):
            output = directory / f"crop.{extension}"
            run([*base, "-d", archive, "-m", "ECT", "--center-lat", "10.12", "--center-lon", "140.08",
                 "--half-range", ".12", "--grid", "--grid-step", ".05", "-o", output])
            check(output.stat().st_size > 100, "METimage encoding")
        run([*base, "-d", archive, "-m", "RT", "--projection", "orthographic", "--center-lat", "10.12",
             "--center-lon", "140.08", "--half-range", ".3", "--grid", "-o", directory / "ortho.png"])
        hashes = []
        for threads in (1, 4, 0):
            output = directory / f"threads-{threads}.png"
            thread_command = base.copy()
            thread_command[thread_command.index("--threads") + 1] = str(threads)
            run([*thread_command, "-d", archive, "-m", "RT", "-o", output])
            hashes.append(hashlib.sha256(output.read_bytes()).hexdigest())
        check(len(set(hashes)) == 1, "METimage thread consistency")
        for mode in ("R", "RMUL", "E", "RT", "RY", "RF", "EC", "ECT", "TSG"):
            job = directory / "job.json"
            output = directory / f"mosaic-{mode}.png"
            specification = {"version": 1, "inputs": [{"data": second.name}, {"data": archive.name}],
                             "mode": mode, "band": 17 if mode == "E" else 0,
                             "map": {"width": 384}, "output": output.name}
            job.write_text(json.dumps(specification))
            run([args.app, "mosaic", "--job", job, "--threads", "4", "--profile", "--scratch-dir", scratch])
            profile = json.loads(Path(str(output) + ".profile.json").read_text())
            check(profile["success"] and profile["source_pixels"] == 96 * 16, "METimage mosaic profile")
            before = output.read_bytes()
            specification["inputs"].reverse()
            job.write_text(json.dumps(specification))
            run([args.app, "mosaic", "--job", job, "--threads", "1", "--scratch-dir", scratch])
            check(before == output.read_bytes(), "METimage mosaic order and thread consistency")
        check(not list(scratch.iterdir()), "METimage scratch cleanup")


if __name__ == "__main__":
    main()
