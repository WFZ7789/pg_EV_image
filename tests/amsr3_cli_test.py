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
    with tempfile.TemporaryDirectory(prefix="pg-ev-amsr3-") as temporary:
        directory = Path(temporary)
        data = directory / "amsr3.nc"
        run([args.fixture, "--fixture", data])
        summary = run([args.app, "inspect", data]).stdout
        check("GOSAT-GW" in summary and "AMSR3" in summary and "Band 20:" in summary, "AMSR3 inspect")
        base = [args.app, "render", "-d", data, "--width", 384, "--threads", 4]
        for mode in ("W84", "TCX", "TCS", "TCR"):
            for scan in ("A", "B", "AB"):
                output = directory / f"{mode}-{scan}.png"
                run([*base, "-m", mode, "--amsr3-scan", scan, "-o", output, "--profile"])
                width, height, pixels = png_pixels(output)
                check(width == 414 and height > 30 and any(pixels), "AMSR3 composite output")
                profile = json.loads(Path(str(output) + ".profile.json").read_text())
                check(profile["success"] and profile["scope"] == "native_swath", "AMSR3 profile")
                expected = 2 * 6 * 16 if scan == "AB" else 6 * 16
                check(profile["source_pixels"] == expected, "A/B source pixel accounting")
        for band in (0, 14, 16, 18, 20):
            output = directory / f"W-{band}.png"
            run([*base, "-m", "W", "-b", band, "-o", output])
        for extension in ("png", "jpg", "tiff"):
            output = directory / f"crop.{extension}"
            run([*base, "-m", "TCS", "--amsr3-scan", "AB", "--center-lat", 10.5,
                 "--center-lon", 140.6, "--half-range", .35, "--grid", "--grid-step", .1, "-o", output])
            check(output.stat().st_size > 100, "Cropped AMSR3 output")
        run([*base, "-m", "TCX", "--amsr3-scan", "AB", "--projection", "orthographic",
             "--center-lat", 10.5, "--center-lon", 140.6, "--half-range", 1, "-o", directory / "ortho.png"])
        for extra in (["-m", "TCN"], ["-m", "W", "-b", 21], ["-m", "W", "--amsr3-scan", "AB"],
                      ["-m", "W84", "--amsr3-scan", "C"],
                      ["-m", "W84", "--center-lat", 70, "--center-lon", 0, "--half-range", 1]):
            result = run([*base, *extra, "-o", directory / "bad.png"], expected=2 if "C" not in extra else 105)
            check(bool(result.stderr), "Invalid AMSR3 request must report an error")


if __name__ == "__main__":
    main()
