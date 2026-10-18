import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import sys
import tarfile
import time


CASES = (
    "mersi_h_1km_e24_full", "mersi_h_250m_e24_repeat",
    "mersi_h_250m_rt_crop15_width2400", "mersi_h_1km_rt_mosaic_3",
    "fy4b_1km_rtec", "fy4b_4km_rtec16", "mwri_h_w8", "giirs_lw200",
    "pmr_surface",
)


def digest(path):
    sha = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(4 * 1024 * 1024), b""):
            sha.update(block)
    return sha.hexdigest()


def save(path, value):
    temporary = path.with_suffix(".partial")
    temporary.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n")
    temporary.replace(path)


def measure(command, directory, stem):
    stdout = directory / (stem + ".stdout.log")
    stderr = directory / (stem + ".stderr.log")
    start = time.perf_counter()
    with stdout.open("wb") as out, stderr.open("wb") as err:
        process = subprocess.Popen(command, stdout=out, stderr=err)
        _, status, usage = os.wait4(process.pid, 0)
        process.returncode = os.waitstatus_to_exitcode(status)
    return {"command": command, "returncode": process.returncode,
            "wall_seconds": time.perf_counter() - start,
            "user_seconds": usage.ru_utime, "system_seconds": usage.ru_stime,
            "peak_rss_bytes": usage.ru_maxrss * (1 if sys.platform == "darwin" else 1024),
            "stdout": str(stdout), "stderr": str(stderr)}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exploratory", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--revision", default="aaf6eef09e45029a3d5c5e7348339727eabb62a3")
    args = parser.parse_args()
    if args.repeats < 1:
        parser.error("repeats must be positive")
    root = Path(__file__).resolve().parents[1]
    destination = args.output.resolve()
    destination.mkdir(parents=True, exist_ok=False)
    revision = subprocess.check_output(["git", "rev-parse", args.revision + "^{commit}"], cwd=root, text=True).strip()
    archive = destination / "source.tar"
    with archive.open("wb") as output:
        subprocess.run(["git", "archive", revision], cwd=root, stdout=output, check=True)
    source = destination / "source"
    source.mkdir()
    with tarfile.open(archive) as packed:
        packed.extractall(source, filter="data")
    shutil.copyfile(root / "tools/refactor_reference.cpp", source / "refactor_reference.cpp")
    with (source / "CMakeLists.txt").open("a") as cmake:
        cmake.write("\nadd_executable(refactor_reference refactor_reference.cpp)\n"
                    "target_link_libraries(refactor_reference PRIVATE fy_pipeline)\n")
    build = destination / "build"
    subprocess.run(["cmake", "-S", str(source), "-B", str(build), "-G", "Ninja",
                    "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"], check=True)
    subprocess.run(["cmake", "--build", str(build), "-j", "4"], check=True)
    subprocess.run(["ctest", "--test-dir", str(build), "--output-on-failure"], check=True)
    manifest = json.loads((args.exploratory / "manifest.json").read_text())
    manifest["revision"] = revision
    manifest["source_sha256"] = digest(archive)
    manifest["executable_sha256"] = digest(build / "pg_EV_image")
    manifest["reference_helper_sha256"] = digest(source / "refactor_reference.cpp")
    manifest["measurement_notes"] = [
        "Fresh processes, sequential workloads, one warmup then measured repeats.",
        "Cache state is uncontrolled; warmup does not imply cold or fully warm NFS cache.",
        "Legacy has no thread-count option; only its existing default execution is measured.",
        "No performance gain is inferred before the new implementation is measured.",
        "PMR is an expected metadata failure, not a successful product baseline.",
    ]
    for item in manifest["inputs"]:
        path = Path(item["path"])
        stat = path.stat()
        if stat.st_size != item["size_bytes"] or stat.st_mtime_ns != item["mtime_ns"]:
            raise RuntimeError("Input changed since exploration: " + str(path))
        item["sha256"] = digest(path)
    save(destination / "manifest.json", manifest)
    exploratory = {item["case"]: item for item in json.loads((args.exploratory / "metrics.json").read_text())}
    records = []
    references = []
    for name in CASES:
        command = exploratory[name]["command"].copy()
        command[0] = str(build / "pg_EV_image")
        if "--job" in command:
            index = command.index("--job") + 1
            job = json.loads(Path(command[index]).read_text())
            job["output"] = str(destination / (name + ".jpg"))
            path = destination / (name + ".json")
            save(path, job)
            command[index] = str(path)
        else:
            index = command.index("-o") + 1
            command[index] = str(destination / (name + Path(command[index]).suffix))
        for run in range(args.repeats + 1):
            record = measure(command, destination, name + "." + str(run))
            record.update(case=name, warmup=run == 0)
            records.append(record)
            save(destination / "metrics.json", records)
            print(name, run, record["returncode"], round(record["wall_seconds"], 3), flush=True)
            expected = 2 if name == "pmr_surface" else 0
            if record["returncode"] != expected:
                raise RuntimeError("Unexpected exit status for " + name)
        if name == "pmr_surface":
            continue
        lossless = command.copy()
        if "--job" in lossless:
            index = lossless.index("--job") + 1
            job["output"] = str(destination / (name + ".png"))
            path = destination / (name + ".lossless.json")
            save(path, job)
            lossless[index] = str(path)
        else:
            index = lossless.index("-o") + 1
            lossless[index] = str(destination / (name + (".tif" if name.endswith("rtec16") else ".png")))
        record = measure(lossless, destination, name + ".lossless")
        if record["returncode"]:
            raise RuntimeError("Lossless reference failed: " + name)
        references.append(record)
        save(destination / "references.json", references)
        if name.startswith("mersi"):
            inputs = job["inputs"] if "--job" in command else [{
                "data": command[command.index("-d") + 1], "geo": command[command.index("-g") + 1]}]
            mode = job["mode"] if "--job" in command else command[command.index("-m") + 1]
            band = "0" if "-b" not in command else command[command.index("-b") + 1]
            for number, item in enumerate(inputs):
                numerical = [str(build / "refactor_reference"), mode, band, item["data"], item["geo"],
                             str(destination / (name + ".frame" + str(number)))]
                record = measure(numerical, destination, name + ".frame" + str(number))
                if record["returncode"]:
                    raise RuntimeError("Numerical reference failed: " + name)
                references.append(record)
                save(destination / "references.json", references)
    artifacts = {}
    for path in destination.iterdir():
        if path.suffix in (".png", ".tif"):
            artifacts[path.name] = digest(path)
        if path.is_dir() and ".frame" in path.name:
            for plane in path.iterdir():
                artifacts[str(plane.relative_to(destination))] = digest(plane)
    save(destination / "reference_hashes.json", artifacts)
    lines = ["# Frozen legacy baseline", "", "Revision: `" + revision + "`.", "",
             "One warmup, " + str(args.repeats) + " measured repeats. Cache state uncontrolled.", "",
             "| Case | Wall median (range), s | Peak RSS median, MiB | Exit |",
             "|---|---:|---:|---:|"]
    for name in CASES:
        samples = [r for r in records if r["case"] == name and not r["warmup"]]
        wall = [r["wall_seconds"] for r in samples]
        rss = [r["peak_rss_bytes"] / 1048576 for r in samples]
        lines.append(f"| {name} | {statistics.median(wall):.3f} ({min(wall):.3f}-{max(wall):.3f}) | "
                     f"{statistics.median(rss):.1f} | {samples[0]['returncode']} |")
    lines += ["", "Compatibility exceptions: GEO fill contamination is retained in these references;",
              "PMR fails on missing Sensor Identification Code before product computation;",
              "legacy has no controllable thread-count flag. Numerical frames are pre-projection,",
              "scene-local values/scales; mosaic common statistics remain a task-level reference.",
              "Other sensors have image references here; physical numerical references are required at migration."]
    (destination / "summary.md").write_text("\n".join(lines) + "\n")
    print("Baseline complete:", destination, flush=True)


if __name__ == "__main__":
    main()
