import argparse
import json
from pathlib import Path
import shutil
import tempfile

from cli_execution_test import check, png_pixels, run


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="pg-ev-giirs-cli-") as temporary:
        directory = Path(temporary)
        run([args.fixture, directory, "giirs"])
        shutil.copy2(directory / "giirs.HDF", directory / "geo.HDF")
        scratch = directory / "scratch"
        scratch.mkdir()
        sentinel = scratch / "keep.txt"
        sentinel.write_text("keep")

        def render(mode, name, threads=4, data="giirs", band=1, extras=(), expected=0):
            output = directory / name / "sequence.png" if mode == "IRall" else directory / (name + ".png")
            command = [args.app, "render", "-d", directory / (data + ".HDF"), "-m", mode,
                       "-b", band, "--width", 256, "-o", output, "--threads", threads,
                       "--profile", "--scratch-dir", scratch]
            run([*command, *extras], expected)
            report = json.loads(Path(str(output) + ".profile.json").read_text())
            check(report["scope"] == "native_spectrum" and report["success"] is (expected == 0), "GIIRS execution path differs")
            check("legacy_render" not in report["phases"], "GIIRS called legacy")
            check(report["scratch_bytes"] == 0 and sorted(scratch.iterdir()) == [sentinel], "GIIRS scratch cleanup failed")
            for usage in (report["buffers"] or {}).values():
                check(usage["current_bytes"] == 0, "GIIRS buffer retained")
            images = sorted(output.parent.glob("[0-9]*.png")) if mode == "IRall" else [output] if output.exists() else []
            if expected:
                check(not images, "Failed GIIRS command produced an image")
                return [], report
            check(len(images) == (5 if mode == "IRall" else 1), "GIIRS frame count differs")
            if mode == "IRall":
                check([image.stem for image in images] == [f"{index:06d}" for index in range(5)], "GIIRS frame numbering differs")
                check(report["source_pixels"] == 120 and report["cache"]["write_bytes"] == 600 and
                      report["cache"]["read_bytes"] == 600 and report["cache"]["current_bytes"] == 0, "GIIRS sequence cache differs")
                check(report["phases"]["product"]["calls"] == 5 and report["phases"]["read"]["calls"] == 5 and
                      report["phases"]["projection"]["calls"] == 2, "GIIRS sequence repeated product work")
            else:
                check(report["source_pixels"] == (80 if mode == "R" else 24) and report["cache"] is None, "GIIRS scalar storage differs")
                check(report["phases"]["product"]["calls"] == 1 and report["phases"]["read"]["calls"] == (4 if mode == "R" else 1),
                      "GIIRS scalar dependencies differ")
            decoded = [png_pixels(image) for image in images]
            check(report["output"] == {"width": decoded[-1][0], "height": decoded[-1][1]}, "GIIRS output dimensions differ")
            return decoded, report

        for mode in ("R", "LW", "MW", "IRall"):
            reference, _ = render(mode, mode)
            for threads in (1, 0):
                pixels, _ = render(mode, mode + "-t" + str(threads), threads=threads)
                check(reference == pixels, "GIIRS pixels depend on threads")
            render(mode, mode + "-masked", data="giirs-masked")
        reference, _ = render("LW", "reference")
        split, _ = render("LW", "split", extras=("-g", directory / "geo.HDF"))
        check(split == reference, "GIIRS independent GEO differs")
        gridded, _ = render("LW", "grid", extras=("--grid", "--grid-step", .2))
        check(gridded != reference, "GIIRS grid was not drawn")
        crop = ("--center-lat", 10.45, "--center-lon", 140.8, "--half-range", .5)
        render("R", "R-crop", extras=crop)
        render("IRall", "IRall-crop", extras=crop)
        render("IRall", "empty-channel", data="giirs-empty-channel")
        render("IRall", "empty-family", data="giirs-empty-family", expected=2)
        render("LW", "outside", extras=("--center-lat", -70, "--half-range", 1), expected=2)
        render("LW", "orthographic", extras=("--projection", "orthographic"), expected=2)
        render("LW", "bad-band", band=3, expected=2)
        render("LW", "negative-band", band=-1, expected=2)
        render("unknown", "unknown", expected=2)


if __name__ == "__main__":
    main()
