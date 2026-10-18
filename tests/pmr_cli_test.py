import argparse
import json
from pathlib import Path
import tempfile

from cli_execution_test import check, png_pixels, run


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="pg-ev-pmr-cli-") as temporary:
        directory = Path(temporary)
        run([args.fixture, directory, "pmr"])
        scratch = directory / "scratch"
        scratch.mkdir()
        sentinel = scratch / "keep.txt"
        sentinel.write_text("keep")

        def render(name, threads=4, data="pmr", geo=None, extras=(), expected=0, native=True, latitude=15, mode="PMR"):
            output = directory / (name + ".png")
            command = [args.app, "render", "-d", directory / (data + ".HDF"), "-m", mode,
                       "--center-lat", latitude, "--center-lon", 140, "--half-range", 1.1,
                       "--width", 256, "-o", output, "--threads", threads, "--profile", "--scratch-dir", scratch]
            if geo:
                command += ["-g", directory / (geo + ".HDF")]
            run([*command, *extras], expected)
            report = json.loads(Path(str(output) + ".profile.json").read_text())
            check(report["success"] is (expected == 0), "PMR success report differs")
            if native:
                check(report["scope"] == "native_swath", "PMR skipped its native entry")
            check("legacy_render" not in report["phases"], "PMR called legacy")
            check(report["scratch_bytes"] == 0 and sorted(scratch.iterdir()) == [sentinel], "PMR scratch cleanup failed")
            for usage in (report["buffers"] or {}).values():
                check(usage["current_bytes"] == 0, "PMR buffer retained")
            if expected:
                check(not output.exists(), "Failed PMR render produced an image")
                return None
            width, height, pixels = png_pixels(output)
            check(report["output"] == {"width": width, "height": height}, "PMR output dimensions differ")
            check(report["source_pixels"] == 25, "PMR did not select the local bounding box")
            check(report["phases"]["product"]["calls"] == 1, "PMR product was recomputed")
            check(report["computed_samples"] > 0 and report["io"]["read_calls"] > 0, "PMR work was not measured")
            return width, height, pixels

        reference = render("missing-sensor")
        for threads in (1, 0):
            check(render("threads-" + str(threads), threads=threads) == reference, "PMR pixels depend on threads")
        check(render("split", data="pmr-data", geo="pmr-geo") == reference, "PMR split geolocation differs")
        check(render("explicit", data="pmr-explicit") == reference, "PMR sensor attribute changed pixels")
        render("masked", data="pmr-masked")
        gridded = render("grid", extras=("--grid", "--grid-step", .5))
        check(gridded != reference, "PMR grid was not drawn")
        render("orthographic", extras=("--projection", "orthographic"))
        render("outside", latitude=-70, expected=2)
        render("wrong-mode", mode="E", expected=2)
        render("no-geo", data="pmr-data", expected=2, native=False)


if __name__ == "__main__":
    main()
