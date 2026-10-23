import argparse
import json
from pathlib import Path
import struct
import tempfile

from cli_execution_test import check, digest, png_pixels, run


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="pg-ev-fy4-cli-") as temporary:
        directory = Path(temporary)
        run([args.fixture, directory, "fy4"])
        scratch = directory / "scratch"
        scratch.mkdir()
        sentinel = scratch / "keep.txt"
        sentinel.write_text("keep")

        def render(mode, name, threads=4, sensor="fy4", auxiliary=False, extras=(), expected=0, extension=".png"):
            output = directory / (name + extension)
            data = directory / (sensor + ".HDF")
            command = [args.app, "render", "-d", data, "-g", data, "-m", mode,
                       "-b", 13 if mode == "E" else 2, "--width", 256, "-o", output,
                       "--threads", threads, "--profile", "--scratch-dir", scratch]
            if auxiliary:
                command += ["-a", directory / (sensor + "-aux.HDF")]
            run([*command, *extras], expected)
            report = json.loads(Path(str(output) + ".profile.json").read_text())
            check(report["scope"] == "native_disk" and report["success"] is (expected == 0), "Wrong FY4 execution path")
            check("legacy_render" not in report["phases"], "FY4 called its legacy entry")
            check(report["scratch_bytes"] == 0 and sorted(scratch.iterdir()) == [sentinel], "FY4 scratch cleanup failed")
            for usage in (report["buffers"] or {}).values():
                check(usage["current_bytes"] == 0, "FY4 buffer was retained")
            if expected:
                check(not output.exists(), "Failed FY4 render produced an image")
                return output, report
            bits = 16 if mode in ("RT16", "RTEC16") else 8
            width, height = struct.unpack(">II", output.read_bytes()[16:24])
            check(output.read_bytes()[24] == bits, "FY4 output bit depth changed")
            check(report["output"] == {"width": width, "height": height}, "FY4 dimensions missing from profile")
            check(report["phases"]["product"]["calls"] == 1 and report["cache"] is None, "FY4 product was recomputed")
            check(report["io"]["read_calls"] > 0 and report["computed_samples"] > 0, "FY4 work was not measured")
            return output, report

        outputs = {}
        for mode in ("E", "R", "Re", "Rr", "EC", "RF", "RY", "RX", "RTpp", "RTBlack", "RTEC", "RT+EC", "AGHI_ALD", "RTEC16", "RT16"):
            output, report = render(mode, mode.replace("+", "plus"))
            outputs[mode] = output
            check(report["output"] == {"width": 356 if mode in ("RTEC16", "RT16") else 556, "height": 580}, "FY4 native layout changed")
            check(report["source_pixels"] == 256 * 256 and "image_rasterization" in report["phases"], "FY4 native raster fast path was skipped")
        black_report = json.loads(Path(str(outputs["RTBlack"]) + ".profile.json").read_text())
        check(black_report["phases"]["rayleigh"]["calls"] == 1 and
              black_report["phases"]["white_balance"]["calls"] == 1, "RTBlack correction was skipped")
        contrast_output, _ = render("RTBlack", "RTBlack-contrast", extras=("-c", "contrast"))
        check(digest(contrast_output) == digest(outputs["RTBlack"]), "RTBlack default is not contrast")
        for curve in ("linear", "srgb", "gamma05"):
            output, _ = render("RTBlack", "RTBlack-" + curve, extras=("-c", curve))
            check(digest(output) != digest(outputs["RTBlack"]), "RTBlack curve was ignored")
        render("RTBlack", "ghi-black", sensor="ghi", expected=2)
        render("RTBlack", "bad-curve", extras=("-c", "unknown"), expected=2)
        for aliases in (("R", "Re"), ("RTEC", "RT+EC", "AGHI_ALD"), ("RTEC16", "RT16")):
            check(len({digest(outputs[mode]) for mode in aliases}) == 1, "FY4 alias results differ")
        for mode in ("RTBlack", "RTEC", "RTEC16"):
            for threads in (1, 0):
                output, _ = render(mode, mode + "-t" + str(threads), threads)
                check(digest(output) == digest(outputs[mode]), "FY4 threads changed native pixels")
            gridded, _ = render(mode, mode + "-grid", extras=("--grid", "--grid-step", 20))
            depth = 16 if mode == "RTEC16" else 8
            width, height, before = png_pixels(outputs[mode], depth)
            _, _, after = png_pixels(gridded, depth)
            stride = 3 * (depth // 8)
            changed = 0
            for row in range(height):
                for col in range(width):
                    offset = (row * width + col) * stride
                    different = before[offset:offset + stride] != after[offset:offset + stride]
                    if row < 162 or row >= 418 or col >= 256:
                        check(not different, "FY4 grid changed annotations or margin")
                    changed += different
            check(changed > 0, "FY4 native grid was not drawn")
        _, report = render("RTEC", "infrared-aux", auxiliary=True)
        check(report["phases"]["read"]["calls"] == 10 and report["phases"]["product"]["calls"] == 1, "FY4 dependencies were read twice")
        for mode in ("RTG", "AGHI_ALD"):
            _, report = render(mode, "ghi-" + mode, sensor="ghi", auxiliary=mode == "RTG")
            expected = {"width": 406, "height": 354} if mode == "RTG" else {"width": 556, "height": 516}
            check(report["output"] == expected, "GHI output grid or layout changed")
        crop = ("--center-lat", 0, "--center-lon", 140, "--half-range", 5)
        first, report = render("RTEC16", "crop-t1", threads=1, extras=crop)
        second, _ = render("RTEC16", "crop-t4", threads=4, extras=crop)
        check(digest(first) == digest(second), "FY4 crop depends on threads")
        check(report["source_pixels"] == 256 * 256 and report["phases"]["read"]["calls"] == 10, "FY4 crop lost full-row dependencies")
        check(report["output"]["width"] == 276 and report["phases"]["projection"]["calls"] == 1, "FY4 crop skipped native projection")
        render("E", "outside", extras=("--center-lat", 0, "--center-lon", -75, "--half-range", 5), expected=2)
        render("RTEC", "orthographic", extras=("--projection", "orthographic"), expected=2)
        render("RTG", "ghi-crop", sensor="ghi", extras=crop, expected=2)
        render("unknown", "unknown", expected=2)
        render("RTEC16", "jpeg", expected=2, extension=".jpg")


if __name__ == "__main__":
    main()
