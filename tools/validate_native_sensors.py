import argparse
import json
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--reference", type=Path, required=True)
    args = parser.parse_args()
    build, baseline, output = args.build.resolve(), args.baseline.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    manifest = json.loads((baseline / "manifest.json").read_text())
    inputs = {Path(item["path"]).name: Path(item["path"]) for item in manifest["inputs"]}
    stem = "FY3H_MERSI_GRAN_L1_20260730_0220_"
    km, quarter, geo = (inputs[stem + name + "_V0.HDF"] for name in ("1000M", "0250M", "GEO1K"))
    mwri = inputs["FY3H_MWRI-_ORBA_L1_20260730_0147_010KM_V0.HDF"]
    records = []

    def run(command):
        command = [str(value) for value in command]
        result = subprocess.run(command, capture_output=True, text=True, timeout=300)
        records.append({"command": command, "returncode": result.returncode,
                        "stdout": result.stdout, "stderr": result.stderr})
        (output / "commands.json").write_text(json.dumps(records, indent=2) + "\n")
        if result.returncode:
            raise RuntimeError("Native sensor validation failed: " + result.stderr + result.stdout)

    run([args.reference.resolve(), "mwri", mwri, "Window Channel/Calibration/EARTH_OBSERVE_BT",
         8, output / "mwri-w8-legacy.f32"])
    run([args.reference.resolve(), "mersi-r", km, "unused", 1, output / "mersi-1km-r1-legacy.f32"])
    cases = [
        ("mersi-1km-e24", "mersi", km, geo, 24, "celsius",
         baseline / "mersi_h_1km_e24_full.frame0/values.f32"),
        ("mersi-250m-e24", "mersi", quarter, geo, 24, "celsius",
         baseline / "mersi_h_250m_e24_repeat.frame0/values.f32"),
        ("mersi-1km-r1", "mersi", km, geo, 1, "reflectance", output / "mersi-1km-r1-legacy.f32"),
        ("mwri-w8", "mwri", mwri, mwri, 8, "kelvin", output / "mwri-w8-legacy.f32"),
    ]
    comparisons = []
    for name, kind, data, location, band, unit, reference in cases:
        prefix = output / name
        run([build / "sensor_reference", kind, data, location, band, prefix, unit])
        validation = output / (name + "-validation.json")
        run([sys.executable, Path(__file__).resolve().parent / "check_sensor_reference.py",
             "--native", prefix, "--reference", reference, "--output", validation])
        result = json.loads(validation.read_text())
        profile = json.loads(Path(str(prefix) + ".profile.json").read_text())
        if profile["buffers"]["scene"]["current_bytes"] != 0:
            raise RuntimeError("Streaming buffer lifetime did not end")
        comparisons.append(result)
        print(name, "common valid", result["common_valid"], "max error", result["maximum_absolute_error"], flush=True)
    (output / "validation-summary.json").write_text(json.dumps(comparisons, indent=2) + "\n")


if __name__ == "__main__":
    main()
