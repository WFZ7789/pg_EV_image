import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import statistics
import tarfile


VARIANTS = ("frozen-default", "native-t1", "native-t4", "native-auto")


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def stats(values):
    return dict(median=statistics.median(values), minimum=min(values), maximum=max(values))


def numeric_leaves(value, prefix=""):
    result = {}
    if isinstance(value, dict):
        for key, child in value.items():
            result.update(numeric_leaves(child, prefix + "." + key if prefix else key))
    elif isinstance(value, (int, float)) and not isinstance(value, bool):
        result[prefix] = value
    return result


def cell(value, scale=1):
    return f"{value['median'] / scale:.3f} [{value['minimum'] / scale:.3f}, {value['maximum'] / scale:.3f}]"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--campaign", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    campaign, baseline = args.campaign.resolve(), args.baseline.resolve()
    root = Path(__file__).resolve().parents[1]
    spec = importlib.util.spec_from_file_location("measurement_driver", root / "tools/benchmark_refactor_v2.py")
    driver = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(driver)
    manifest = json.loads((campaign / "manifest.json").read_text())
    frozen = json.loads((baseline / "manifest.json").read_text())
    records = json.loads((campaign / "metrics.json").read_text())
    expected = {(case, variant, iteration) for case in driver.CASES for variant in VARIANTS for iteration in range(6)}
    actual = [(run["case"], run["variant"], run["iteration"]) for run in records]
    if len(actual) != len(expected) or set(actual) != expected:
        raise RuntimeError("Missing, duplicate, or unexpected benchmark runs")
    for name, sha in (("native", manifest["native_sha256"]), ("frozen", manifest["frozen_sha256"])):
        if digest(campaign / "executables" / name) != sha:
            raise RuntimeError("Campaign executable changed")
    if manifest["frozen_sha256"] != frozen["executable_sha256"]:
        raise RuntimeError("Incorrect frozen baseline")
    if digest(root / "tools/benchmark_refactor_v2.py") != manifest["driver_sha256"]:
        raise RuntimeError("Measurement driver changed")
    if digest(campaign / "native-source.tar.gz") != manifest["source_sha256"]:
        raise RuntimeError("Source snapshot changed")
    with tarfile.open(campaign / "native-source.tar.gz", "r:gz") as packed:
        for member in packed.getmembers():
            if member.isfile() and packed.extractfile(member).read() != (root / member.name).read_bytes():
                raise RuntimeError("Current measured source differs: " + member.name)
    if "CMAKE_BUILD_TYPE:STRING=Release" not in (campaign / "CMakeCache.txt").read_text():
        raise RuntimeError("Campaign is not a Release build")
    for command in json.loads((campaign / "compile_commands.json").read_text()):
        if any(flag not in command["command"] for flag in ("-O3", "-DNDEBUG", "-std=gnu++23")):
            raise RuntimeError("Unexpected native compilation flags")
    if manifest["inputs"] != [{key: item[key] for key in ("path", "size_bytes", "sha256")} for item in frozen["inputs"]]:
        raise RuntimeError("Input manifest differs")
    frozen_commands = {run["case"]: run["command"] for run in json.loads((baseline / "metrics.json").read_text()) if run["warmup"]}
    groups = {}
    for run in records:
        case, variant, iteration = run["case"], run["variant"], run["iteration"]
        native = variant != "frozen-default"
        expected_code = 2 if case == "pmr_surface" and not native else 0
        if run.get("validated") is not True or run["returncode"] != expected_code or run["warmup"] != (iteration == 0):
            raise RuntimeError("Invalid completed run")
        command = run["command"]
        if command[0] != str(campaign / "executables" / ("native" if native else "frozen")):
            raise RuntimeError("Run did not use the campaign executable")
        arguments = command[1:]
        if native:
            threads = {"native-t1": 1, "native-t4": 4, "native-auto": 0}[variant]
            if arguments[-5:] != ["--threads", str(threads), "--profile", "--scratch-dir", str(campaign / "scratch")]:
                raise RuntimeError("Incorrect native execution options")
            arguments = arguments[:-5]
            profile = run["profile"]
            if not profile["success"] or profile["requested_threads"] != threads or profile["scratch_bytes"]:
                raise RuntimeError("Invalid profile or scratch leak")
            if profile["thread_limit"] != (threads or manifest["cpu_count"]):
                raise RuntimeError("Incorrect effective thread limit")
            if any(value["current_bytes"] for value in profile["buffers"].values()):
                raise RuntimeError("Unreleased native buffers")
        original = frozen_commands[case][1:]
        if "--job" in original:
            index = original.index("--job") + 1
            old_job = json.loads(Path(original[index]).read_text())
            new_job = json.loads(Path(arguments[index]).read_text())
            image = Path(new_job.pop("output"))
            if image.suffix != Path(old_job.pop("output")).suffix:
                raise RuntimeError("Mosaic output format changed")
            if old_job != new_job:
                raise RuntimeError("Mosaic workload changed")
        else:
            index = original.index("-o") + 1
            image = Path(arguments[index])
            if image.suffix != Path(original[index]).suffix:
                raise RuntimeError("Output format changed")
        if arguments[:index] + arguments[index + 1:] != original[:index] + original[index + 1:]:
            raise RuntimeError("Workload arguments changed")
        if expected_code == 0:
            if not image.is_relative_to(campaign) or digest(image) != run["image_sha256"] or driver.image_info(image) != run["image"]:
                raise RuntimeError("Output image changed")
            if native and profile["output"] != {key: run["image"][key] for key in ("width", "height")}:
                raise RuntimeError("Profile output dimensions differ")
            if case == "fy4b_4km_rtec16" and (run["image"]["depth"] != 16 or run["image"]["compression"] != 8):
                raise RuntimeError("RTEC16 encoding parameters differ")
        groups.setdefault((case, variant), []).append(run)
    details = []
    for case in driver.CASES:
        hashes = {run["image_sha256"] for run in records if run["case"] == case and run["variant"] != "frozen-default"}
        if len(hashes) != 1:
            raise RuntimeError("Native repeats or thread variants differ: " + case)
        for variant in VARIANTS:
            runs = groups[case, variant]
            warmup = next(run for run in runs if run["warmup"])
            measured = [run for run in runs if not run["warmup"]]
            item = dict(case=case, variant=variant, warmup_wall_seconds=warmup["wall_seconds"],
                        image=warmup.get("image"), returncode=warmup["returncode"])
            for key in ("wall_seconds", "user_seconds", "system_seconds", "peak_rss_bytes"):
                item[key] = stats([run[key] for run in measured])
            item["cpu_seconds"] = stats([run["user_seconds"] + run["system_seconds"] for run in measured])
            if variant != "frozen-default":
                leaves = [numeric_leaves(run["profile"]) for run in measured]
                if any(set(leaf) != set(leaves[0]) for leaf in leaves):
                    raise RuntimeError("Profile schema changed within group")
                item["profile"] = {key: stats([leaf[key] for leaf in leaves]) for key in leaves[0]}
                item["process_source_pixels_per_second"] = stats([run["profile"]["source_pixels"] / run["wall_seconds"] for run in measured])
                item["process_output_pixels_per_second"] = stats([run["image"]["width"] * run["image"]["height"] / run["wall_seconds"] for run in measured])
            details.append(item)
    lookup = {(item["case"], item["variant"]): item for item in details}
    comparisons = []
    for case in driver.CASES:
        old, single, new = (lookup[case, variant] for variant in ("frozen-default", "native-t1", "native-auto"))
        comparisons.append(dict(case=case, frozen_product_available=bool(old["image"]),
                                same_output_dimensions=bool(old["image"]) and all(old["image"][key] == new["image"][key] for key in ("width", "height")),
                                coverage_correction=case.startswith("mersi"),
                                frozen_to_native_auto_wall_ratio=old["wall_seconds"]["median"] / new["wall_seconds"]["median"] if old["image"] else None,
                                native_single_to_auto_wall_ratio=single["wall_seconds"]["median"] / new["wall_seconds"]["median"],
                                frozen_to_native_auto_rss_ratio=old["peak_rss_bytes"]["median"] / new["peak_rss_bytes"]["median"] if old["image"] else None))
    lines = ["# V2 正式性能与成本报告", "", "测量日期：" + manifest["created_utc"] + "。", "",
             "Apple M4，10 核，16 GB；Apple Clang Release `-O3 -DNDEBUG`、C++23。旧版固定提交 `" + frozen["revision"] + "`。",
             "同一批只读 NFS 输入先核对 SHA-256，输出与任务缓存位于本地 `/private/tmp`。两个可执行文件均复制固定后测量。",
             "顺序启动独立进程，每项每配置预热一次、测量五次，共 216 次进程运行；没有同时运行构建或测试。",
             "缓存状态未受控，哈希核对和预热会影响缓存，不能称为冷缓存。后台系统负载未隔离。旧版没有线程选项。", "",
             "以下数值为中位数 [最小值, 最大值]。总时间、CPU、峰值 RSS 使用父进程 `wait4` 测量，包含进程启动与退出；",
             "内部阶段、读取与内存模型来自原生报告。阶段可能重叠，不能相加；所有缓冲已释放、任务缓存已清理。", "",
             "## 总时间、CPU 与 RSS", "", "| 负载 | 配置 | 首次/预热 s | 测量墙钟 s | CPU s | 峰值 RSS MiB |",
             "|---|---|---:|---|---|---|"]
    for item in details:
        lines.append(f"| {item['case']} | {item['variant']} | {item['warmup_wall_seconds']:.3f} | {cell(item['wall_seconds'])} | {cell(item['cpu_seconds'])} | {cell(item['peak_rss_bytes'], 1048576)} |")
    lines += ["", "## 画幅与比较边界", "", "| 负载 | 旧版尺寸 | 新版尺寸 | 编码 | 旧/新自动线程墙钟中位数比 |", "|---|---|---|---|---:|"]
    for case in driver.CASES:
        old, new = lookup[case, "frozen-default"], lookup[case, "native-auto"]
        size = lambda item: f"{item['image']['width']}×{item['image']['height']}" if item["image"] else "入口失败（exit 2）"
        ratio = f"{old['wall_seconds']['median'] / new['wall_seconds']['median']:.3f}" if old["image"] else "不适用"
        image = new["image"]
        lines.append(f"| {case} | {size(old)} | {size(new)} | {image['codec']} / {image['depth']} bit | {ratio} |")
    comparable = [case for case in driver.CASES if lookup[case, "frozen-default"]["image"]]
    shorter = [case for case in comparable if lookup[case, "native-auto"]["wall_seconds"]["median"] < lookup[case, "frozen-default"]["wall_seconds"]["median"]]
    longer = [case for case in comparable if case not in shorter]
    lines += ["", "本次自动线程墙钟较短：" + ("、".join(shorter) or "无") + "。",
              "本次自动线程墙钟较长或相等：" + ("、".join(longer) or "无") + "。"]
    lines += ["", "比值大于 1 表示本次新版墙钟较短，小于 1 表示较长；这是整项负载的观察值，不能直接归因于并行或算法。",
              "MERSI 修正 GEO1K FillValue、越界 DN 和定位有效性，画幅/覆盖可能改变；面积覆盖采样也改变执行工作。",
              "完整 250M 的有效定位为 65,386,500/65,536,000，旧结果仅按物理范围检查为 65,386,520；",
              "共同有效域的定位、物理值、颜色已在功能验收中单独对照，详见执行记录。覆盖修正与画幅缩减不计为算法提速。",
              "PMR 旧版缺少元数据属性即退出，其耗时不是产品基线，新版验证的是实际产品路径。",
              "JPEG 均使用同一 OpenCV 默认编码参数；RTEC16 两版均为 RGB16 TIFF、Adobe Deflate（compression=8）。",
              "每项新版全部 18 次输出的 SHA-256 相同，因此重复及 1/4/自动线程的编码图像一致；这不要求新采样像素等于旧图缩放。", "",
              "## 原生读取、计算与吞吐", "", "| 负载/配置 | 逻辑读取 MiB | 解码 MiB | 读调用 | 读取样本 | 计算样本 | 源像元 M/s | 输出像元 M/s |", "|---|---:|---:|---:|---:|---:|---:|---:|"]
    native = [item for item in details if "profile" in item]
    for item in native:
        p = item["profile"]
        values = [p[key]["median"] for key in ("io.logical_bytes", "io.decoded_bytes", "io.read_calls", "io.read_samples", "computed_samples")]
        lines.append(f"| {item['case']}/{item['variant']} | {values[0]/1048576:.3f} | {values[1]/1048576:.3f} | {values[2]:g} | {values[3]:g} | {values[4]:g} | {item['process_source_pixels_per_second']['median']/1e6:.3f} | {item['process_output_pixels_per_second']['median']/1e6:.3f} |")
    lines += ["", "读取为应用级逻辑/解码计数，不是物理 NFS 流量；旧版没有对应计数，未作推测。吞吐分母使用完整进程墙钟。", "",
              "## 原生内存与任务缓存", "", "单位 MiB，均为五次峰值的中位数；逐次值及范围见 JSON。", "",
              "| 负载/配置 | 画布 | 归属 | 当前景 | bins | 统计 | 工作 tile | 编码缓冲 | 同时追踪峰值 | 缓存写入/读取 | 缓存磁盘峰值 |", "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for item in native:
        p = item["profile"]
        values = [p[f"buffers.{key}.peak_bytes"]["median"] / 1048576 for key in ("framebuffer", "ownership", "scene", "bins", "statistics", "worker_tiles", "encoder")]
        cache = [p.get("cache." + key, {"median": 0})["median"] / 1048576 for key in ("write_bytes", "read_bytes", "peak_bytes")]
        lines.append("| " + item["case"] + "/" + item["variant"] + " | " + " | ".join(f"{value:.3f}" for value in values) + f" | {p['tracked_buffer_peak_bytes']['median']/1048576:.3f} | {cache[0]:.3f}/{cache[1]:.3f} | {cache[2]:.3f} |")
    lines += ["", "各类别独立峰值不一定同时出现，不能相加得到同时峰值或 RSS。归属信息可内嵌于工作 tile，无独立归属分配时为 0。",
              "模型追踪显式应用缓冲的逻辑容量，并非常驻物理页；编码器内部、第三方分配器及运行库未全部归入类别，`encoder_internal_bytes=null`；",
              "编码缓冲 0 不表示编码器不占内存。进程 RSS 包含这些开销，不能用显式追踪峰值替代。", "",
              "## 阶段耗时", "", "单位秒，为五次中位数；各阶段调用次数、范围及缓存读写次数均保存在 details JSON。", "",
              "| 负载/配置 | read | calibration | resampling | product | projection | bins 两阶段 | rasterization/image_rasterization | annotations | encode |", "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for item in native:
        p = item["profile"]
        phase = lambda key: p.get("phases." + key + ".wall_seconds", {"median": 0})["median"]
        values = [phase(key) for key in ("read", "calibration", "resampling", "product", "projection")]
        bins = f"{phase('bin_counts'):.3f}/{phase('bin_fill'):.3f}"
        raster = phase("rasterization") or phase("image_rasterization")
        lines.append("| " + item["case"] + "/" + item["variant"] + " | " + " | ".join(f"{value:.3f}" for value in values) + f" | {bins} | {raster:.3f} | {phase('annotations'):.3f} | {phase('encode'):.3f} |")
    lines += ["", "阶段不存在时显示 0。规则网格使用 image_rasterization，扫描带使用 rasterization；",
              "完整阶段集合（含 prepare_scene、cache_projection、cache_write、shared_statistics 等）见 details JSON。"]
    lines += ["", "## 已知回退与后续定位", "",
              "本轮完成迁移和验收，不代表全部负载获得加速。MERSI 的面积覆盖栅格化，以及 FY4 1KM RTEC 的窗口读取/重采样，",
              "是本次原生报告中需要继续优化的阶段。读取计时包括适配器与 HDF5 操作，不能仅据此归因为 NFS 网络；",
              "HDF5 分块、窗口粒度、数据集缓存等具体原因仍需专门测量。上述定位没有修改本轮固定可执行文件或混合测量版本。"]
    lines += ["", "## 复现与证据", "", "```bash", "python3 tools/benchmark_refactor_v2.py \\",
              "  --baseline .work/refactor-v2/baseline-aaf6eef-20261011 \\",
              "  --app .work/refactor-v2/build-native-clean/pg_EV_image \\", "  --output /private/tmp/新的运行目录", "",
              "python3 tools/report_refactor_v2.py \\", "  --baseline .work/refactor-v2/baseline-aaf6eef-20261011 \\",
              "  --campaign /private/tmp/新的运行目录 \\", "  --report docs/REFACTOR_V2_PERFORMANCE.md", "```", "",
              "本次目录：`" + str(campaign) + "`；manifest、源码归档、固定二进制、全部命令/日志、metrics、summary、details、audit 和逐次图像均在该目录。",
              "原生可执行文件 SHA-256：`" + manifest["native_sha256"] + "`。",
              "旧版可执行文件 SHA-256：`" + manifest["frozen_sha256"] + "`。",
              "验收不设置 RSS 或提速比例硬门槛；正确性、采样质量与架构边界由独立功能测试及执行记录证明。", ""]
    (campaign / "details.json").write_text(json.dumps(details, indent=2) + "\n")
    (campaign / "comparison.json").write_text(json.dumps(comparisons, indent=2) + "\n")
    audit = dict(records=len(records), measured_runs=180, warmups=36, unique_expected_runs=True,
                 same_workload_arguments=True, native_thread_and_repeat_images_identical=True,
                 output_hashes_validated=True, executable_hashes_validated=True,
                 measured_source_matches_workspace=True, reporter_sha256=digest(Path(__file__)), all_cases_passed=True)
    (campaign / "audit.json").write_text(json.dumps(audit, indent=2) + "\n")
    args.report.write_text("\n".join(lines))
    print(json.dumps(audit, indent=2))


if __name__ == "__main__":
    main()
