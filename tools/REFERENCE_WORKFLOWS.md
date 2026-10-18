# 独立参考工具

正式 CMake 目标不编译旧 reader、渲染器、bridge 或 CLI。产品测试使用
[`tests/reference_v1`](../tests/reference_v1/README.md) 中的冻结数值和颜色参考。

`refactor_reference.cpp` 和 `legacy_sensor_reference.cpp` 是冻结基线专用导出器，
只在显式指定的旧源码归档副本中构建，不链接进应用或默认测试。
`freeze_refactor_baseline.py` 默认使用固定提交 `aaf6eef09e45029a3d5c5e7348339727eabb62a3`，
不再将当前重构后的 HEAD 当作旧实现；可用 `--revision` 显式指定兼容的旧提交。

如需重新验证真实数据的 reader，先从已经核对哈希的基线归档构建独立程序：

```bash
python3 tools/build_frozen_sensor_reference.py \
  --baseline .work/refactor-v2/baseline-aaf6eef-20261011 \
  --output .work/refactor-v2/frozen-sensor-reference-新的标识
```

生成的 `build/frozen_sensor_reference` 只使用归档内的头文件；源归档、导出器与可执行文件的
SHA-256 及编译命令写入其 `manifest.json`。输出目录必须是新目录，已有冻结基线不会被修改。

`validate_native_sensors.py` 和 `validate_fy4_sensors.py` 分别接受新的 `--reference` 参数，
其值为上述程序的绝对路径；`--build` 指向当前原生构建，`--baseline` 指向输入/数值清单。
两套实现分别生成物理值后进行独立比较。

清理后可检查正式构建和二进制：

```bash
python3 tools/check_native_architecture.py \
  --build .work/refactor-v2/build-native-clean \
  --output .work/refactor-v2/stage5-acceptance-20261018/architecture.json
```

此检查验证源码、实际编译命令与可执行文件符号中没有旧执行路径，核心/产品层没有应用库类型，
HDF5 对象仅出现在传感器实现。它不代替完整 CTest、真实图像验证或阶段 6 的新载荷扩展验收。
