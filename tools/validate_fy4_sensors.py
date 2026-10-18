import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

import numpy as np


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--reference", type=Path, required=True)
    args = parser.parse_args()
    build, output = args.build.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    manifest = json.loads((args.baseline / "manifest.json").read_text())
    inputs = [item for item in manifest["inputs"] if "FY4B-_AGRI" in Path(item["path"]).name]
    if len(inputs) != 3:
        raise RuntimeError("Expected frozen 1KM, 4KM and GEO FY4 inputs")
    validation = [{"path": item["path"], "sha256": digest(Path(item["path"])),
                   "frozen_sha256": item["sha256"]} for item in inputs]
    (output / "input-validation.json").write_text(json.dumps(validation, indent=2) + "\n")
    if any(item["sha256"] != item["frozen_sha256"] for item in validation):
        raise RuntimeError("FY4 input differs from frozen manifest")
    km = next(Path(item["path"]) for item in inputs if "1000M" in item["path"])
    four = next(Path(item["path"]) for item in inputs if "4000M" in item["path"] and "FDI-" in item["path"])
    geo = next(Path(item["path"]) for item in inputs if "GEO-" in item["path"])
    records = []

    def run(command):
        command = list(map(str, command))
        print("START", command, flush=True)
        result = subprocess.run(command, capture_output=True, text=True, timeout=600)
        records.append({"command": command, "returncode": result.returncode,
                        "stdout": result.stdout, "stderr": result.stderr})
        (output / "commands.json").write_text(json.dumps(records, indent=2) + "\n")
        if result.returncode:
            raise RuntimeError("FY4 validation command failed: " + result.stdout + result.stderr)

    comparisons = []
    for name, data, band in (("fy4-4km-r1", four, 1), ("fy4-4km-e13", four, 13), ("fy4-1km-r2", km, 2)):
        prefix = output / name
        reference = output / (name + "-legacy.f32")
        run([args.reference.resolve(), "fy4", data, "unused", band, reference])
        run([build / "sensor_reference", "fy4", data, geo, band, prefix, "native"])
        report = output / (name + "-validation.json")
        run([sys.executable, Path(__file__).with_name("check_sensor_reference.py"),
             "--native", prefix, "--reference", reference, "--output", report])
        comparisons.append(json.loads(report.read_text()))
        print(name, "validated", flush=True)

    prefix = output / "fy4-4km-geometry"
    run([build / "sensor_reference", "fy4", four, geo, "native", prefix, "geometry"])
    mask = np.memmap(str(prefix) + ".u8", dtype="u1", mode="r")
    coordinates = {}
    for axis in ("lat", "lon"):
        reference = output / ("fy4-4km-legacy-" + axis + ".f32")
        run([args.reference.resolve(), "fy4-" + axis, four, "Data/NOMChannel13", "unused", reference])
        native_path = Path(str(prefix) + (".latitude.f32" if axis == "lat" else ".longitude.f32"))
        native = np.memmap(native_path, dtype="=f4", mode="r")
        legacy = np.memmap(reference, dtype="=f4", mode="r")
        if native.size != mask.size or legacy.size != mask.size:
            raise RuntimeError("FY4 geometry dimensions differ")
        stats = {"samples": int(mask.size), "valid": 0, "mask_mismatch": 0,
                 "maximum_absolute_error": 0.0, "over_tolerance": 0}
        for begin in range(0, mask.size, 1024 * 1024):
            selection = slice(begin, begin + 1024 * 1024)
            a, b, m = native[selection], legacy[selection], mask[selection] != 0
            if np.any(mask[selection] > 1) or np.any(m & ~np.isfinite(a)):
                raise RuntimeError("Invalid native disk coordinates")
            common = m & np.isfinite(b)
            stats["valid"] += int(common.sum())
            stats["mask_mismatch"] += int((m != np.isfinite(b)).sum())
            error = a[common].astype("f8") - b[common]
            if axis == "lon":
                error = (error + 180) % 360 - 180
            error = np.abs(error)
            tolerance = 1e-5 + 2e-6 * np.abs(b[common].astype("f8"))
            stats["over_tolerance"] += int((error > tolerance).sum())
            if error.size:
                stats["maximum_absolute_error"] = max(stats["maximum_absolute_error"], float(error.max()))
        stats["sha256"] = {str(path): digest(path) for path in (native_path, reference)}
        coordinates[axis] = stats
    (output / "geometry-validation.json").write_text(json.dumps(coordinates, indent=2) + "\n")
    if any(stats["mask_mismatch"] or stats["over_tolerance"] or not stats["valid"] for stats in coordinates.values()):
        raise RuntimeError("FY4 disk geometry differs from legacy navigation")
    (output / "validation-summary.json").write_text(json.dumps({"bands": comparisons, "geometry": coordinates}, indent=2) + "\n")
    print("FY4 sensor and disk navigation validation passed", flush=True)


if __name__ == "__main__":
    main()
