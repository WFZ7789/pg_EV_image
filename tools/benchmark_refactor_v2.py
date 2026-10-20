import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import statistics
import struct
import subprocess
import sys
import tarfile
import time


CASES = ("mersi_h_1km_e24_full", "mersi_h_250m_e24_repeat", "mersi_h_250m_rt_crop15_width2400",
         "mersi_h_1km_rt_mosaic_3", "fy4b_1km_rtec", "fy4b_4km_rtec16", "mwri_h_w8", "giirs_lw200", "pmr_surface")


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def save(path, value):
    temporary = path.with_suffix(path.suffix + ".partial")
    temporary.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n")
    temporary.replace(path)


def image_info(path):
    with path.open("rb") as source:
        header = source.read(8)
        if header[:2] == b"\xff\xd8":
            source.seek(2)
            while True:
                if source.read(1) != b"\xff":
                    raise RuntimeError("Invalid JPEG marker")
                code = source.read(1)
                while code == b"\xff":
                    code = source.read(1)
                length = int.from_bytes(source.read(2), "big")
                if code[0] in (0xC0, 0xC1, 0xC2):
                    depth, height, width = struct.unpack(">BHH", source.read(5))
                    return dict(width=width, height=height, depth=depth, codec="jpeg", compression="OpenCV default")
                source.seek(length - 2, 1)
        if header == b"\x89PNG\r\n\x1a\n":
            source.seek(16)
            width, height, depth = struct.unpack(">IIB", source.read(9))
            return dict(width=width, height=height, depth=depth, codec="png")
        order = "<" if header[:2] == b"II" else ">" if header[:2] == b"MM" else None
        if not order or struct.unpack(order + "H", header[2:4])[0] != 42:
            raise RuntimeError("Unsupported image header")
        source.seek(struct.unpack(order + "I", header[4:])[0])
        count = struct.unpack(order + "H", source.read(2))[0]
        entries = [struct.unpack(order + "HHII", source.read(12)) for _ in range(count)]
        tags = {}
        for tag, kind, size, location in entries:
            if tag not in (256, 257, 258, 259):
                continue
            code = "H" if kind == 3 else "I" if kind == 4 else None
            if not code:
                raise RuntimeError("Unsupported TIFF tag type")
            length = struct.calcsize(code) * size
            if length <= 4:
                data = struct.pack(order + "I", location)[:length]
            else:
                source.seek(location)
                data = source.read(length)
            tags[tag] = struct.unpack(order + code * size, data)[0]
        return dict(width=tags[256], height=tags[257], depth=tags[258], codec="tiff", compression=tags[259])


def measure(command, directory, stem):
    stdout, stderr = directory / (stem + ".stdout.log"), directory / (stem + ".stderr.log")
    start = time.perf_counter()
    with stdout.open("wb") as out, stderr.open("wb") as err:
        process = subprocess.Popen(command, stdout=out, stderr=err)
        try:
            _, status, usage = os.wait4(process.pid, 0)
        except BaseException:
            process.terminate()
            process.wait()
            raise
        process.returncode = os.waitstatus_to_exitcode(status)
    return dict(command=command, returncode=process.returncode, wall_seconds=time.perf_counter() - start,
                user_seconds=usage.ru_utime, system_seconds=usage.ru_stime,
                peak_rss_bytes=usage.ru_maxrss * (1 if sys.platform == "darwin" else 1024),
                stdout=str(stdout), stderr=str(stderr))


def summarize(records):
    groups = {}
    for record in records:
        if record["warmup"]:
            continue
        groups.setdefault((record["case"], record["variant"]), []).append(record)
    result = []
    for (case, variant), runs in groups.items():
        item = dict(case=case, variant=variant, measured_runs=len(runs), returncodes=[run["returncode"] for run in runs])
        for key in ("wall_seconds", "user_seconds", "system_seconds", "peak_rss_bytes"):
            values = [run[key] for run in runs]
            item[key] = dict(median=statistics.median(values), minimum=min(values), maximum=max(values))
        item["images"] = [run.get("image") for run in runs]
        item["image_hashes"] = sorted({run["image_sha256"] for run in runs if "image_sha256" in run})
        result.append(item)
    return result


def remap_path(value, inputs, data_root):
    path = Path(value)
    return str(data_root / path.name) if data_root and str(path) in inputs else value


def remap_job(job, inputs, data_root):
    result = dict(job)
    result["inputs"] = [{key: remap_path(value, inputs, data_root) for key, value in item.items()}
                        for item in job["inputs"]]
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--before-app", type=Path)
    parser.add_argument("--data-root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cases", nargs="+", choices=CASES, default=list(CASES))
    parser.add_argument("--resume", action="store_true")
    args = parser.parse_args()
    root, baseline, app, output = Path(__file__).resolve().parents[1], args.baseline.resolve(), args.app.resolve(), args.output.resolve()
    before_app = args.before_app.resolve() if args.before_app else None
    data_root = args.data_root.resolve() if args.data_root else None
    frozen = json.loads((baseline / "manifest.json").read_text())
    old_app = baseline / "build/pg_EV_image"
    if digest(old_app) != frozen["executable_sha256"]:
        raise RuntimeError("Frozen executable differs")
    old_cases = {record["case"]: record["command"] for record in json.loads((baseline / "metrics.json").read_text()) if record["warmup"]}
    if not args.resume:
        output.mkdir(parents=True, exist_ok=False)
        (output / "executables").mkdir()
        shutil.copy2(app, output / "executables/native")
        shutil.copy2(old_app, output / "executables/frozen")
        if before_app: shutil.copy2(before_app,output / "executables/before")
        validation = []
        for item in frozen["inputs"]:
            original = Path(item["path"])
            path = data_root / original.name if data_root else original
            print("Hashing", path.name, flush=True)
            sha = digest(path)
            if sha != item["sha256"] or path.stat().st_size != item["size_bytes"]:
                raise RuntimeError("Frozen input differs: " + str(path))
            validation.append(dict(path=str(path), original_path=str(original),size_bytes=path.stat().st_size,sha256=sha))
        source_archive = output / "native-source.tar.gz"
        with tarfile.open(source_archive, "w:gz") as packed:
            for directory in (root / "src", root / "include"):
                for path in sorted(directory.rglob("*")):
                    if path.is_file() and not path.name.startswith("._"):
                        packed.add(path, arcname=str(path.relative_to(root)))
            packed.add(root / "CMakeLists.txt", arcname="CMakeLists.txt")
            packed.add(Path(__file__), arcname="tools/benchmark_refactor_v2.py")
        shutil.copy2(app.parent / "compile_commands.json", output / "compile_commands.json")
        shutil.copy2(app.parent / "CMakeCache.txt", output / "CMakeCache.txt")
        manifest = dict(created_utc=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), platform=platform.platform(),
                        cpu_count=os.cpu_count(), compiler=subprocess.check_output(["/usr/bin/c++", "--version"], text=True),
                        native_sha256=digest(app), frozen_sha256=digest(old_app),
                        before_sha256=digest(before_app) if before_app else None,source_sha256=digest(source_archive),
                        driver_sha256=digest(Path(__file__)), inputs=validation,
                        notes=["Fresh sequential processes; one warmup and five measured runs per variant.",
                               "Timed native runs use normal commands; each final native variant has one separate profiling run.",
                               "Hashes and warmups may warm caches; cache state is uncontrolled, never called cold cache.",
                               "Frozen default has no thread option; native variants are 1, 4, and automatic.",
                               "Inputs, outputs and native scratch are local to this campaign, outside NFS.",
                               "GEO fill corrections can change extent/work; do not infer algorithmic speedups from that reduction.",
                               "PMR frozen metadata failure is recorded separately and is not a speed baseline."])
        save(output / "manifest.json", manifest)
        records = []
    else:
        manifest = json.loads((output / "manifest.json").read_text())
        if digest(app) != manifest["native_sha256"] or digest(old_app) != manifest["frozen_sha256"] or \
           bool(before_app) != bool(manifest.get("before_sha256")) or \
           (before_app and digest(before_app) != manifest["before_sha256"]):
            raise RuntimeError("Cannot mix executable versions in a resumed campaign")
        if digest(Path(__file__)) != manifest["driver_sha256"]:
            raise RuntimeError("Cannot change measurement driver during a resumed campaign")
        for item in manifest["inputs"]:
            if digest(Path(item["path"])) != item["sha256"]:
                raise RuntimeError("Input changed before resume")
        records = json.loads((output / "metrics.json").read_text()) if (output / "metrics.json").exists() else []
        if any(not record.get("validated") for record in records):
            raise RuntimeError("Campaign has an unvalidated run; inspect it before starting a new campaign")
    executables = [("native",manifest["native_sha256"]),("frozen",manifest["frozen_sha256"])]
    if manifest.get("before_sha256"): executables.append(("before",manifest["before_sha256"]))
    for name, expected in executables:
        if digest(output / "executables" / name) != expected:
            raise RuntimeError("Campaign executable changed")
    scratch = output / "scratch"
    scratch.mkdir(exist_ok=True)
    completed = {(item["case"], item["variant"], item["iteration"]) for item in records}
    input_paths = {item["original_path"] for item in manifest["inputs"]}
    variants = [("frozen-default","frozen",None,False)]
    if manifest.get("before_sha256"): variants.append(("native-before-auto","before",None,False))
    variants += [("native-t1","native",1,True),("native-t4","native",4,True),("native-auto","native",None,True)]
    for case in args.cases:
        original = [remap_path(value,input_paths,data_root) for value in old_cases[case][1:]]
        for variant, executable_name, threads, profile_variant in variants:
            directory = output / case / variant
            directory.mkdir(parents=True, exist_ok=True)
            def command_for(stem, profile):
                command = list(original)
                if "--job" in command:
                    job_path_source = Path(old_cases[case][old_cases[case].index("--job") + 1])
                    job = remap_job(json.loads(job_path_source.read_text()),input_paths,data_root)
                    image = directory / (stem + Path(job["output"]).suffix)
                    job["output"] = str(image)
                    job_path = directory / (stem + ".json")
                    save(job_path, job)
                    command[command.index("--job") + 1] = str(job_path)
                else:
                    index = command.index("-o") + 1
                    image = directory / (stem + Path(command[index]).suffix)
                    command[index] = str(image)
                command = [str(output / "executables" / executable_name), *command]
                if threads is not None: command += ["--threads", str(threads)]
                if profile: command += ["--profile", "--scratch-dir", str(scratch)]
                return command,image
            for iteration in range(6):
                if (case, variant, iteration) in completed:
                    continue
                stem = "warmup" if iteration == 0 else "repeat-" + str(iteration)
                command,image = command_for(stem,False)
                save(output / "current-run.json", dict(case=case, variant=variant, iteration=iteration, command=command))
                print("Starting", case, variant, stem, flush=True)
                result = measure(command, directory, stem)
                result.update(case=case, variant=variant, iteration=iteration, warmup=iteration == 0)
                records.append(result)
                save(output / "metrics.json", records)
                if result["returncode"] == 0:
                    result["image"] = image_info(image)
                    result["image_sha256"] = digest(image)
                    result["image_bytes"] = image.stat().st_size
                    if case == "fy4b_4km_rtec16" and (result["image"]["depth"] != 16 or result["image"]["compression"] != 8):
                        raise RuntimeError("RTEC16 must use RGB16 TIFF with Adobe Deflate for both versions")
                expected_code = 2 if executable_name == "frozen" and case == "pmr_surface" else 0
                result["validated"] = result["returncode"] == expected_code
                save(output / "metrics.json", records)
                save(output / "summary.json", summarize(records))
                print("Completed", case, variant, stem, round(result["wall_seconds"], 3), "s",
                      round(result["peak_rss_bytes"] / 1048576, 1), "MiB", "exit", result["returncode"], flush=True)
                if result["returncode"] != expected_code:
                    raise RuntimeError("Benchmark exited unexpectedly; see saved logs")
            if profile_variant:
                command,image = command_for("profile",True)
                record_path = directory / "profile-run.json"
                if record_path.exists():
                    profiled = json.loads(record_path.read_text())
                    if profiled["command"] != command:
                        raise RuntimeError("Profile command differs during resume")
                else:
                    save(output / "current-run.json", dict(case=case,variant=variant,profile=True,command=command))
                    print("Profiling",case,variant,flush=True)
                    profiled = measure(command,directory,"profile")
                    if profiled["returncode"] != 0:
                        raise RuntimeError("Profile run exited unexpectedly; see saved logs")
                    profiled["profile"] = json.loads(Path(str(image) + ".profile.json").read_text())
                    profiled["image"] = image_info(image)
                    profiled["image_sha256"] = digest(image)
                    profiled["image_bytes"] = image.stat().st_size
                    save(record_path,profiled)
                profile = profiled["profile"]
                if profile["success"] is not True or profile["scratch_bytes"] != 0:
                    raise RuntimeError("Native profile failed or leaked its cache")
                if any(buffer["current_bytes"] for buffer in profile["buffers"].values()):
                    raise RuntimeError("Native buffer retained after render")
                if digest(image) != profiled["image_sha256"] or image_info(image) != profiled["image"]:
                    raise RuntimeError("Profile output changed")
    save(output / "completion.json", dict(completed_selected_cases=list(args.cases), records=len(records),
                                          native_sha256=manifest["native_sha256"],
                                          profiles=len(args.cases) * 3,
                                          all_cases=len(records) == len(CASES) * len(variants) * 6))


if __name__ == "__main__":
    main()
