import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--reader", type=Path, required=True)
    parser.add_argument("--data", type=Path, default=Path("testdata"))
    parser.add_argument("--output", type=Path, default=Path("output/amsr3-validation"))
    args = parser.parse_args()
    app, reader = args.app.resolve(), args.reader.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    result = subprocess.run([reader, "--samples", args.data], capture_output=True, text=True, timeout=120, check=True)
    (args.output / "reader.txt").write_text(result.stdout)
    files = sorted(args.data.glob("GGWAM3*.nc"))
    records = []

    def render(data, name, mode, extra=(), threads=4, width=768, extension="png"):
        path = args.output / f"{name}.{extension}"
        command = [app, "render", "-d", data, "-m", mode, "--width", str(width), "--threads", str(threads),
                   "--profile", "-o", path, *map(str, extra)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=120)
        if result.returncode:
            raise RuntimeError(f"{command}\n{result.stdout}\n{result.stderr}")
        profile = json.loads(Path(str(path) + ".profile.json").read_text())
        if not profile["success"] or not path.is_file():
            raise RuntimeError(f"Invalid render: {path}")
        records.append({"name": name, "input": data.name, "mode": mode, "threads": threads,
                        "wall_seconds": profile["wall_seconds"], "peak_rss_bytes": profile["process_peak_rss_bytes"],
                        "io": profile["io"], "source_pixels": profile["source_pixels"], "output": profile["output"]})
        return path

    for data in files:
        subprocess.run([app, "inspect", data], capture_output=True, text=True, timeout=30, check=True)
        render(data, data.stem + "-W84-AB", "W84", ["--amsr3-scan", "AB"])
        print(f"Validated {data.name}: inspect / full-orbit W84 AB", flush=True)
    representatives = [next(p for p in files if "A024" in p.name), next(p for p in files if "D004" in p.name)]
    for data in representatives:
        for band in range(21):
            render(data, data.stem + f"-W-{band:02}", "W", ["-b", band])
        for mode in ("W84", "TCX", "TCS", "TCR"):
            for scan in ("A", "B", "AB"):
                render(data, data.stem + f"-{mode}-{scan}", mode, ["--amsr3-scan", scan])
        print(f"Validated {data.name}: all 21 bands / 4 composites x A,B,AB", flush=True)
    data = representatives[0]
    crop = ["--amsr3-scan", "AB", "--center-lat", 14, "--center-lon", 140, "--half-range", 12]
    for extension in ("png", "jpg", "tiff"):
        render(data, "crop-grid-" + extension, "TCS", [*crop, "--grid", "--grid-step", 5], extension=extension)
    render(data, "orthographic-grid", "TCX", [*crop, "--projection", "orthographic", "--grid", "--grid-step", 5])
    hashes = []
    for threads in (1, 4, 0):
        output = render(data, f"TCS-AB-threads-{threads}", "TCS", ["--amsr3-scan", "AB"], threads=threads)
        hashes.append(hashlib.sha256(output.read_bytes()).hexdigest())
    if len(set(hashes)) != 1:
        raise RuntimeError("Real AMSR3 1/4/auto thread output differs")
    render(data, "TCS-AB-width2400", "TCS", ["--amsr3-scan", "AB"], width=2400)
    (args.output / "results.json").write_text(json.dumps({"files": len(files), "reader_windows": 168,
        "executable_sha256": hashlib.sha256(app.read_bytes()).hexdigest(),
        "thread_sha256": hashes[0], "renders": records}, indent=2) + "\n")
    print(f"Passed {len(records)} renders; results: {args.output / 'results.json'}", flush=True)


if __name__ == "__main__":
    main()
