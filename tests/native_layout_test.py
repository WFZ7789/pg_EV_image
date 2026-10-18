import argparse
import math
from pathlib import Path
import tempfile

from cli_execution_test import check, png_pixels, run


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="pg-ev-native-layout-") as temporary:
        directory = Path(temporary)
        run([args.fixture, directory])
        data = directory / "FY3H_MERSI_20260618_0050_1000M_V0.HDF"
        for width in (768, 960):
            scale = width / 3840
            top = math.floor(162 * scale + .5)
            body_height = width * 31 // 63
            right = math.floor(300 * scale + .5)
            results = []
            for enabled in (False, True):
                output = directory / f"map-{width}-{enabled}.png"
                command = [args.app, "render", "-d", data, "-g", data, "-m", "E", "-b", 24,
                           "--width", width, "--threads", 4, "-o", output]
                run([*command, "--grid", "--grid-step", 2] if enabled else command)
                columns, rows, pixels = png_pixels(output)
                check((columns, rows) == (width + right, body_height + 2 * top), "Native scaled layout differs")
                results.append(pixels)
            changed = 0
            for row in range(rows):
                for col in range(columns):
                    index = (row * columns + col) * 3
                    different = results[0][index:index + 3] != results[1][index:index + 3]
                    if col >= width or row < top or row >= top + body_height:
                        check(not different, "Grid changed a title, footer, coordinate or colorbar")
                    changed += different
            check(changed > 0, "Grid was not drawn")
            check(any(results[0][:top * columns * 3]), "Native title region is blank")
            check(any(results[0][(rows - top) * columns * 3:]), "Native footer region is blank")
        for sensor, mode in (("pmr", "PMR"), ("giirs", "LW"), ("fy4", "RTEC16")):
            inputs = directory / sensor
            run([args.fixture, inputs, sensor])
            summary = run([args.app, "inspect", inputs / (sensor + ".HDF")]).stdout
            check("Satellite:" in summary and "Sensor:" in summary and mode in summary, "Native inspect lost product modes")


if __name__ == "__main__":
    main()
