# 第二次重构执行记录

本轮按 `REFACTOR_V2_EXECUTION_PLAN.tmp.md` 的阶段门推进。阶段 0 已冻结参考，
阶段 1 的核心契约、执行选项和报告已实现。阶段 2 已增加原生读取、数值、GEO、
产品计算、面积覆盖栅格器、最终版式、拼接入口和任务数值缓存，已通过阶段 2 验证；
阶段 3 的 FY4 产品及正式入口、阶段 4 的 PMR/GIIRS 迁移、阶段 5 的旧路径清理已通过验证；
阶段 6 的功能、扩展性、正式性能测量及最终审计已完成；八项可比负载均出现墙钟回退，
详见 [性能与成本报告](REFACTOR_V2_PERFORMANCE.md)。

## 阶段 0：冻结参考

源码参考为 `aaf6eef09e45029a3d5c5e7348339727eabb62a3`。冻结产物位于
`.work/refactor-v2/baseline-aaf6eef-20261011/`，不进入版本控制。
`source.tar` 来自该提交的 `git archive`，独立构建不依赖之后的工作区修改。
参考导出器仅加入冻结构建，不进入正式程序。

复现命令：

```bash
python3 tools/freeze_refactor_baseline.py \
  --exploratory /private/tmp/pg-ev-refactor-baseline-wi0vx_ao \
  --output .work/refactor-v2/baseline-新的运行标识
```

脚本检查输入大小和修改时间，记录 SHA-256、源码归档和可执行文件哈希。
每个固定负载顺序运行一次预热和五次测量，记录进程退出码、墙钟、CPU 时间及峰值 RSS；
测量 JPEG 工作负载后额外保存 PNG 或 16 位 TIFF。
MERSI 单景和三段拼接的各景保存定标/产品数值、原始定位、有效性、颜色及局部显示尺度。
这些平面使用 `frame.json` 中记录的本机字节序，float32 为四字节 IEEE 754，颜色顺序为 BGR。
所有参考图像和平面另有 SHA-256 清单。

### 兼容例外和测量边界

- 旧版 GEO1K 填充值会参与插值并污染经纬度边界。冻结参考保留这一行为；新路径修正时
  应单独报告有效覆盖、画幅和工作量变化。
- PMR 文件在读取缺失的 `Sensor Identification Code` 时返回 2，尚未计算产品。
  该记录是入口失败参考，阶段 4 必须验证实际 `Geo_Fields/SLV` 产品路径。
- 旧程序没有线程数选项，阶段 0 测量旧程序默认运行行为。阶段 6 对新程序测量
  1、4、自动线程，不能将旧版运行伪称为这些线程配置。
- 每次运行使用新进程；NFS 和系统缓存没有人为清空。预热及重复运行不代表受控冷缓存。
- 数值平面位于投影前，拼接各景的局部显示尺度不等于任务共同拉伸尺度；共同显示行为
  通过固定任务和无损拼接图共同留存。
- FY4、MWRI、GIIRS 此阶段冻结无损图像；各产品的物理数值参考在对应迁移阶段补齐。
- 阶段 0 不声称获得任何提速。新增核心也须在真正接入产品路径后才能评价性能。

## 阶段 1：核心契约

实现位于 `include/core/` 和 `src/core/`，由独立的 `fy_core` 目标构建。
公共接口只使用 C++ 标准库类型，oneTBB 仅出现在执行实现中；核心构建只依赖 oneTBB
和系统线程库。本阶段验收时，正式程序仍通过原有产品和渲染路径成图。

- `Raster<T>` 拥有连续数据，只能移动；移动保留缓冲地址。`RasterView<T>` 使用 span，
  检查尺寸、跨度、长度、窗口边界和整数溢出。移动所有者后，视图仍借用已转移的缓冲；
  最后一个所有者必须存活到消费完成，不能从临时 Raster 取视图。
- `SwathGeometry`、`NativeDiskGeometry`、`ObservationGeometry` 按几何能力描述定位，
  不包含载荷分派。位置有效性与数值有效性独立；无效原始坐标可保留，但不得标为有效。
  `ProductFrame` 保存物理平面与共享的只读定位，不要求先生成 RGB。
- `SensorAdapter` 描述元数据、通道轴、定标来源、单位、填充值、网格配对和窗口读取。
  `AdapterRegistry` 打开 `SensorSession`，由 `IoChannel` 工作线程执行构造、查询、
  读取、结果校验及销毁。所有使用非线程安全 HDF5 的适配器必须共用进程级
  `IoChannel::shared()`。通道提供同步 `run` 和异步 `submit`，销毁时排空队列；
  当前 legacy 调用仍串行运行在原有入口线程。
- `ProductPlan` 描述输入网格、halo、行前缀/全景参数依赖、统计域与百分位秩、帧列表、
  位深和光谱帧通道。这里只建立契约，尚不执行统计、产品公式或光谱多帧生成。
- `DisplayDescriptor` 将着色与数值解耦，并显式描述颜色空间。
  `LayoutTransform` 从最终主体尺寸计算 framebuffer 尺寸、边距和版式锚点，不分配画布。
  新 renderer 尚未使用这一变换。
- `ExecutionContext` 使用一个 oneTBB arena，默认每块 256 个完整扫描行；任务结束或
  异常返回前等待计算结束。`ScratchDirectory` 仅清理本次创建的随机子目录，保留父目录
  和其他文件。读取/计算双缓冲和磁盘数值缓存属于阶段 2。
- `ProfileReport` 提供总墙钟、进程 CPU、进程峰值 RSS、可重叠阶段、I/O 与计算计数、
  源/输出吞吐、缓存字节及各类缓冲的当前/峰值预留。总缓冲峰值按同时预留计算，
  不是分类峰值相加，也不等于 RSS。预留由后续分配点接入，不代表自动追踪所有分配。
  墙钟与 CPU 从报告对象创建计至 `finish()`，不包含 CLI 解析、报告写盘和最终清理；
  RSS 是进程生命周期内的峰值，不是任务前后 RSS 的差值。

`render` 与 `mosaic` 均接受 `--threads N`、`--profile`、`--scratch-dir PATH`。
线程数 0 为自动；显式线程数同时限制 oneTBB 与旧 OpenCV 计算。`--profile` 在输出参数后
附加 `.profile.json` 保存报告，失败任务也会尝试保存 `success: false`。阶段 1 的报告范围为
`legacy_compatibility`，只实测总墙钟、CPU、RSS、旧渲染阶段和当前缓存字节；
尚未插桩的 I/O、数值计算、缓冲与输出尺寸为 `null`，不能把它们当作零开销。
本阶段未进行新旧性能收益比较。

### 验证和当前环境限制

2026-10-17 在独立目录 `.work/refactor-v2/build-stage1/` 进行 Release 验证，
避免现有 Makefiles 缓存与 Ninja 预设冲突。原有三个功能测试保留，新测试包括：

- `core_model_test`：移动与借用、带跨度窗口、越界/溢出、无效坐标、独立掩膜、全无效产品、
  计划依赖、最终尺寸布局及三种几何契约。
- `core_adapter_test`：并发调用串行执行、嵌套 I/O、异常后继续使用、错误波段/窗口/几何、
  session 移动赋值及构造失败后的线程内销毁。
- `core_execution_test`：1、4、自动线程下扫描行恰好处理一次、并行异常、缓存清理、
  计数、重叠阶段及同时存活缓冲峰值。
- `cli_execution_test`：合成 MERSI HDF5 单景/拼接在 1、4、自动线程下 PNG 哈希相同；
  JSON 报告可解析，失败状态正确，不清理无关缓存文件。该测试需要 Python 解释器。

额外使用 CLI 测试脚本的 `--baseline` 参数，确认相同合成输入的新旧程序 PNG 哈希相同。
独立核心通过 `-DFY_BUILD_APP=OFF` 构建，并用 ASan/UBSan 运行核心测试；
它不查找或链接 HDF5、OpenCV、CLI11。

本阶段验收结果：Release CTest 7/7 通过，独立核心 ASan/UBSan CTest 3/3 通过。

冻结参考重新校验通过：8 份无损图像、6 份数值帧、源码及可执行文件哈希均匹配。
阶段 1 验证时 `/Volumes/nfs` 未挂载，真实卫星输入不可访问。上述合成验证不等于阶段 2 所要求的
真实 MERSI/MWRI 迁移验收，也不更新 2026-10-11 的性能基线。

复现验证：

```bash
cmake -S . -B .work/refactor-v2/build-stage1 -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build .work/refactor-v2/build-stage1 -j 4
ctest --test-dir .work/refactor-v2/build-stage1 --output-on-failure
python3 tests/cli_execution_test.py \
  --app .work/refactor-v2/build-stage1/pg_EV_image \
  --fixture .work/refactor-v2/build-stage1/cli_fixture \
  --baseline .work/refactor-v2/baseline-aaf6eef-20261011/build/pg_EV_image
cmake -S . -B .work/refactor-v2/build-core-sanitize -G Ninja \
  -DFY_BUILD_APP=OFF -DCMAKE_BUILD_TYPE=Debug \
  '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Wpedantic' \
  -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined
cmake --build .work/refactor-v2/build-core-sanitize -j 4
ctest --test-dir .work/refactor-v2/build-core-sanitize --output-on-failure
```

## 阶段 2：MERSI/MWRI

2026-10-17 NFS 恢复挂载后，已核对 MERSI 1KM、250M、GEO1K 和 MWRI 四份真实输入的
SHA-256，均与冻结清单一致。输入校验和数值产物保存在 `.work/refactor-v2/native-reference/`。

### 已迁移的读取和数值模块

- `fy_sensors` / `sensors/hdf.hpp` 注册 MERSI 和 MWRI 原生适配器，不依赖 OpenCV 或
  legacy。MERSI 描述配置表中实际存在的 1KM/250M 波段、太阳角、微光和辅助 250M 文件；
  辅助热红外波段的亮温转换系数仍来自主数据，保留既有 EC 配对语义。
- MWRI 提供 FY-3D 的三种通道轴排列，以及 FY-3H Window/Sounding 网格；轴检测同时
  检查定位尺寸，歧义时报错。窗口直接使用 HDF5 hyperslab，不读取完整三维波段立方体。
- 读取窗口返回原始 DN 和独立有效掩膜。描述中 `unit` 表示 DN，`calibrated_unit` 表示
  定标输出单位；`CalibrationDescriptor` 明确系数、来源和变换。
  定标在 I/O 通道外用 oneTBB 执行，保留旧公式的 float 运算和 Planck 转换顺序。
- 波段表和通用物理常数已移入 `include/products/` 的标准库头文件；旧路径通过薄头文件
  共用这些定义，没有复制一份参数表或另改物理公式。
- `stream_calibrated` 通过两个拥有数据的块衔接读取与计算；块和内存预留一起移动，
  消费者保留块时继续计入缓冲量。预取与定标可重叠，消费/读取失败时等待预取结束再返回。
- `resample_cubic` 按整景坐标计算 halo；分块输出与整景输出一致。定位填充值在插值前
  屏蔽，非零权重邻居无效时输出无效；经度在邻域展开后插值，再规范化，避免日期线插值到 0°。
  原始定位保留，不写入投影坐标。

### 真实数值证据

| 负载 | 共同有效像元 | 最大绝对误差 | 超出约定容差 |
|---|---:|---:|---:|
| MERSI 1KM E24 摄氏温度 | 4,094,000 | 0 | 0 |
| MERSI 250M E24 摄氏温度 | 65,464,000 | 0 | 0 |
| MERSI 1KM 波段 1 基础反射定标，尚未太阳修正 | 4,096,000 | 0 | 0 |
| MWRI Window 8 原生网格亮温 K | 1,051,440 | 0 | 0 |

E24 对照冻结的数值平面；反射率与 MWRI 对照未修改的旧读取公式导出器。
比较脚本逐块检查 `1e-5 + 2e-6 × |参考值|`，记录掩膜差异及数值文件 SHA-256。
两个 E24 负载分别新增屏蔽 2,000 / 72,000 个 FillValue 或有效范围外 DN；
它们在旧路径中得到有限数值，不能当作数值对照通过的样本。

完整 250M GEO 的 65,536,000 个位置中，新规则保留 65,386,500 个有效位置；
旧结果仅按物理经纬度范围检查得到 65,386,520 个。共同有效域上，纬度最大差
`7.62939453125e-6` 度，经度周期差最大 `6.103515625e-5` 度，均在约定容差内。
有效纬度范围为约 5.0026° 至 26.2835°，以 165°E 展开后的经度范围为约
149.5138° 至 180.0430°。这些是定位有效性和坐标对照，尚不是新渲染器画幅或覆盖面积验收。

数值复测：

```bash
python3 tools/validate_native_sensors.py \
  --build .work/refactor-v2/build-native-clean \
  --reference .work/refactor-v2/frozen-sensor-reference-20261018/build/frozen_sensor_reference \
  --baseline .work/refactor-v2/baseline-aaf6eef-20261011 \
  --output .work/refactor-v2/native-reference-新的运行标识
```

`commands.json` 记录实际命令和退出状态，`*-validation.json` 记录逐项误差、掩膜与哈希，
`validation-summary.json` 汇总四个负载。参考导出器用于离线比较，不是正式程序的回退路径。

### 验证与剩余工作

新增测试覆盖了多项式/线性/Planck 定标、缺失/过短系数、三种 MWRI 通道轴、
int16 填充值与有效范围、FY-3D 独立 A/B 系数、FY-3E 二维微光、主辅定标来源、分块 halo、日期线、全无效定位、
读取/计算重叠、缓冲保留和异常退出。HDF5 对象计数在全部会话关闭后回到初始状态。
2026-10-18 Release CTest 14/14 通过；独立核心及缓存 ASan/UBSan 8/8 通过；
产品、HDF5 原生适配及 CLI 测试在单独的 ASan/UBSan 构建中 3/3 通过。

### 产品和扫描带栅格器

`fy_products` 使用标准库和 `fy_core`，迁入全部 10 种 MERSI 与 8 种 MWRI 模式。
`load_product` 按计划读取、定标和对齐依赖网格，保留独立数值掩膜及原始定位；
产品准备和显示回调在 I/O 通道外执行。旧颜色接口通过薄包装复用标准库色阶。
全景统计包含百分位、极值、RF 背景均值和有序比值、EC/EK 太阳参数及阴影邻域。
场景缓冲预留随所有权转移和缩放；MWRI 按区域包围盒读取波段，再准备产品与局部统计。
其 8 种模式的合成裁剪结果均与旧产品一致，保留原始窗口及整景列数供覆盖裁决使用。
EC 的同一源波段不重复读取，EK 不读取公式中未使用的水汽通道。

`core/render.hpp` 的通用扫描带栅格器将 source quad 索引到目标 tile，
由每个 tile 独占输出，按显式扫描覆盖策略裁决重叠。它按面积累计映射后的颜色，
最终量化为 BGR8/BGR16；投影不会覆盖原始经纬度，renderer 不判断载荷名称。
测试使用解析面积和独立密集面积参考，覆盖窄条、非线性色阶、重叠、扫描中心、
日期线和投影背面；1、4、自动线程及不同 tile 尺寸的像素与覆盖率一致。

`swath_products_test` 使用合成 HDF5 对照旧产品函数，18 种模式全部像元的显示量化差为 0。
该测试验证产品公式，不等于正式入口或最终图像验收；旧包装与新产品共用色阶定义，
也不能单凭这项测试证明色阶迁移与冻结图像一致。
另用 `check_palette_reference` 对照独立冻结的两个 E24 数值/颜色平面：
1KM 4,096,000 个、250M 65,536,000 个样本的全部 BGR 通道均完全一致。
该工具仅验证温度的 `color_rbtop_wv` 色阶，不用于 RT 等合成 RGB 参考。

2026-10-18 已重新访问 `/Volumes/nfs/Data/FY_2026_TC_GLOBE/Dolphin/II/` 的真实输入，
对 1KM MERSI 的 9 个非 LL 模式及 MWRI 的 8 个模式执行旧产品对照。
命令、标准输出和退出码保存在
`.work/refactor-v2/native-reference/product-validation-20261018/final-commands.json`。
17/17 项的共同有效像元显示差不超过 1，其中 16 项为 0，EK 为 1。
此前的失败日志保留在同目录 `commands.json`；修复前后如下：

| 模式 | 修复前最大量化差 | 修复后证据 |
|---|---:|---|
| MERSI E24 | 206 | 显示差 0；共同有效物理值最大误差 0，显示上界恢复为 40 |
| MERSI RF | 3 | 显示差 0 |
| MERSI EK | 39 | 显示差不超过 1 |

原因是把带无效掩膜的有限值直接变成 NaN，改变了产品原有的“仅排除 NaN”统计域。
现在仅对 `include_masked_finite` 明确启用的产品保留这些有限值用于统计，掩膜始终为无效，绘制仍跳过。
默认定标及 GEO 插值保持严格无效值处理；新增定标、重采样及 E/RF/EK FillValue 回归验证此边界。

该真实对照可通过以下形式独立重现；失败会返回非零状态：

```bash
.work/refactor-v2/build-stage1/swath_products_test mersi DATA GEO E 24
.work/refactor-v2/build-stage1/swath_products_test mwri DATA DATA W 8
```

本次还通过 LSP MCP 对新增产品执行层和核心测试执行 clangd 诊断，六个文件无错误或警告。
静态诊断、合成测试和真实对照分别记录，不能相互替代。

### 单景正式入口

`src/native_swath.cpp` 已接入 MERSI/MWRI 单景 `render`，直接分配最终宽度画布并按比例绘制
标题、页脚、坐标、色条和可选 grid，不构造基准大画布后缩放。OpenCV 编码借用最终 BGR
缓冲；入口探测和原生 HDF5 操作均在串行 I/O 通道执行。单景失败不调用旧产品函数。
报告范围为 `native_swath`，记录读取、定标、产品、投影、bins、栅格化、标注和编码阶段，
以及最终尺寸和显式缓冲预留。`buffer_accounting` 明确为 `explicit_reservations`，
`encoder_internal_bytes` 为 null，不能将编码器内部未知分配理解为零；RSS 包含进程总峰值。

CLI 测试在 1、4、自动线程下验证单景 PNG 一致、主体宽度与缩放边距、grid 只修改地图主体、
grid 图层内存计数、正交投影裁剪、全景统计保留、失败报告和临时目录清理。
真实 MERSI 1KM/250M E24、MWRI W8、正交投影 EK 已通过正式 CLI 生成并目视检查，
命令、PNG 和性能 JSON 位于 `.work/refactor-v2/native-reference/native-render-20261018/`。
自动范围对日期线按连续经度展开，保留东经 149 至 180 度的标签，避免整景标签平移 360 度。

| 单景诊断命令 | 主体宽度 | 最终 PNG 尺寸 |
|---|---:|---:|
| MERSI 1KM E24，含 grid | 800 | 863 × 625 |
| MERSI 250M E24 | 800 | 819 × 577 |
| MWRI W8，区域裁剪及 grid | 512 | 552 × 698 |
| MERSI EK，正交投影裁剪及 grid | 512 | 552 × 556 |

250M 的 65,536,000 个源像元全部处理。宽度 800 的诊断暴露了空白片段反复裁剪的开销：
修复前约 160.8 秒，修复后两次观察为 34.3 / 34.7 秒，最后一次栅格化约 28.4 秒。
优化缓存片段包围盒、原地跳过不相交片段，并在整个像素被覆盖时直接合成。
修复前后 PNG 解码的 819 × 577 像素所有通道完全一致；命令、报告与逐通道比较保存在
同目录 `fragment-optimization-command.json`、`final-commands.json` 和 `final-image-comparison.json`。
新增密集小四边形及孔洞的解析测试仍通过。该诊断不是固定负载的预热一次加五次正式基准，
没有控制缓存或并发构建影响，不能据此声称相对于旧版端到端提速；较大输出仍须优化和测量。

### 拼接正式入口

正常 `mosaic` 已由 `src/app.cpp` 调用 `render_native_mosaic`，报告范围为 `native_mosaic`。
它验证数据/GEO 配对、卫星、载荷、分辨率、文件名时间及重复观测，按时间和路径排序。
每景只进行一次 HDF5 波段窗口读取、定标及产品准备，数值和独立掩膜写入任务私有缓存，
不生成中间 RGB。共同 E 极值和 RT/RY/RMUL 的 65,536 桶直方图在这一阶段汇总。
共同百分位使用既有 99% 向上秩及 [0,8] 截断规则；达到最高桶时仍报错。

`CachedSwath` 保存分块数值与投影顶点。几何准备一次只加载一景，
`rasterize_swaths` 使用有预算的 LRU 读取源块；默认每个活动工作线程的块数据预算为 16 MiB。
缓存块可以重复读取，HDF5 波段数据和产品计算不执行第二遍。缓存包含浮点值与掩膜，
着色与最终量化仍在目标 tile 中完成。临时文件在正常返回和异常退出时由所有者清理。

`cache` 报告区分缓存逻辑读写字节、调用次数、景数和文件逻辑大小峰值，
`shared_display_scale` 记录共同显示范围；原始输入的 HDF5 计数仍在 `io`，两者不混用。
新 `swath_cache_test` 验证有限缓存预算下的块边界、孔洞、16 位逐像素对照、
两种重叠策略、混合内存/缓存来源、不同线程与 tile 尺寸，以及损坏缓存和文件名冲突。
CLI 测试验证 E/RT/RY/RMUL 的共同统计、每景实际计算次数、输入倒序、重复时间、缺失 GEO，
以及两景缓存已建立后统计失败时的清理。旧图缩放的字节比较已改为最终版式与采样契约测试；
可选的旧可执行文件参数仅作单独成图冒烟检查。

真实三段 RT 使用冻结的 2026-06-18 00:50、00:55、01:00 输入，主体宽 2400、正交投影，
输出为 2588 × 2602。处理 12,288,000 个源像元，产品准备 3 次、波段窗口读取 96 次；
缓存报告景数为 3，返回时缓存文件逻辑字节为 0。
从三个独立冻结数值帧重新计算的共同 99% 拉伸值为 `0.921766996383667`，与原生任务完全一致。
本轮还重新核对三段数据/GEO 及 250M 裁剪数据/GEO 共 8 份输入的 SHA-256，全部匹配冻结清单。
命令、图像、报告和独立统计验证位于 `.work/refactor-v2/native-reference/native-mosaic-20261018/`。

三段 RT 在 1、4、自动线程下的 PNG 字节完全一致，证据为 `thread-validation.json`。
固定中心 16°N、165°E、半径 7.5°、主体宽 2400 的真实 250M RT 裁剪已生成 2459 × 2464 PNG，
65,536,000 个源像元参与必要的全景统计；图像及报告保存在上述目录，并已目视检查。

### EC 辅助数据与阶段门

真实 FY3H 2026-07-30 02:20 的 1KM 主数据、250M 辅助数据及 GEO1K 三份输入已重新核对 SHA-256，
全部匹配冻结清单。`swath_products_test` 接受可选的第六个参数作为辅助文件，
用旧产品函数独立比较投影前颜色：65,160,000 个共同有效像元最大量化差为 1。
同一组合通过原生 CLI 生成 20° × 20°、主体宽 800 的 EC 图，最终尺寸为 819 × 819，
产品准备仅一次，图像已目视检查。证据位于
`.work/refactor-v2/native-reference/ec-aux-validation-20261018/`。

阶段 2 的读取/定标、全部 18 种产品、主辅配对、共同统计、缓存、最终版式、采样及并行一致性
已有上述单元、合成 CLI 和真实数据证据；据此进入阶段 3。
本阶段没有预热一次加五次的正式新旧性能结论，固定负载性能测量保留在阶段 6。
阶段 2 验收时 FY4、PMR/GIIRS 迁移和 legacy 删除尚未完成，后续进展见下。

## 阶段 3：FY4 AGRI/GHI

### 已迁移的读取与导航

`register_hdf_adapters` 新增 `fy4`，识别 AGRI/GHI 的 NOM 通道及对应 CAL 查找表，
按原始 DN 的窗口读取，不加载完整波段立方体。定标描述记录表来源、输出单位和输出填充值；
`calibrate` 在 I/O 通道外执行查表，表内有限填充值与越界索引均保持无效。
`retain_finite` 可保留有限填充值供显式启用的既有统计使用，但不会恢复掩膜。
主文件、辅助文件及不同分辨率的太阳天顶角、太阳方位角、卫星天顶角使用独立网格，
不在 reader 中隐式插值。数据/GEO/aux 的卫星、载荷及观测时间继续验证。

完整圆盘导航从 `NOMCenterLon`、`NOMSatHeight`、`dEA`、`dObRecFlat` 和扫描角属性读取。
适配器只返回带窗口偏移的 `NativeDiskGeometry`，没有在串行 I/O 线程展开整幅经纬度。
`core/navigation.hpp` 的通用椭球交点函数负责计算；它不包含卫星或载荷判断。
目前只有完整方形、从行列 0 开始、中心纬度为 0 且属性齐全的网格提供圆盘导航；
其他网格使用 `ImageGeometry` 保留原生画幅坐标，不伪造地理定位；
地理裁剪和经纬网仍要求完整导航。GHI 的验证范围见下文产品与 CLI 记录。

### 独立真实对照

输入使用冻结的 FY4B 2026-09-27 05:30 AGRI 1KM、4KM 及 GEO 4KM 文件，
三份 SHA-256 均已重新核对并匹配。参考导出器调用未修改的旧 `read_fy4` 和 NOM 导航函数，
没有链接新适配器。实际执行命令、数值、掩膜、哈希和报告位于
`.work/refactor-v2/native-reference/fy4-sensors-20261018/`。

| 负载 | 全部源像元 | 共同有效像元 | 最大绝对误差 |
|---|---:|---:|---:|
| 4KM 通道 1 反射率 | 7,551,504 | 5,788,684 | 0 |
| 4KM 通道 13 亮温 K | 7,551,504 | 5,784,544 | 0 |
| 1KM 通道 2 反射率 | 120,824,064 | 92,564,044 | 0 |

4KM 圆盘的 7,551,504 个位置中，5,760,944 个与参考同为有效，掩膜差异为 0；
有效纬度及按周期比较的经度误差均为 0。经度规范化到 [-180,180]，因此经度文件
不要求 SHA-256 与旧版相同。这些是读取、定标及几何证据，尚不是 FY4 产品或成图验收。

复现：

```bash
python3 tools/validate_fy4_sensors.py \
  --build .work/refactor-v2/build-native-clean \
  --reference .work/refactor-v2/frozen-sensor-reference-20261018/build/frozen_sensor_reference \
  --baseline .work/refactor-v2/baseline-aaf6eef-20261011 \
  --output .work/refactor-v2/native-reference/fy4-sensors-新的运行标识
```

`fy4_adapter_test` 覆盖 LUT 越界、表内有限填充值、两种无效值策略、角度缩放、主辅来源、
配对失败、错误尺寸/查找表/导航参数，以及窗口与完整圆盘的坐标一致性。
新适配器及通用导航通过 LSP MCP 诊断，0 错误、0 警告。
最新 Release 全套 CTest 15/15、独立核心 ASan/UBSan 8/8，以及产品、两组 HDF5 适配器和
原生 CLI 的 ASan/UBSan 4/4 全部通过。真实数值已在定标填充值策略补齐后重新执行；
`build-validation.json` 留存对应源文件和可执行文件哈希，并确认旧 `read.h` 与 HEAD 相同。
### 产品、最终画幅和正式入口

`src/products/fy4.cpp` 已迁入已有模式及别名，`RTEC`／`RTEC16` 使用相同的日间校正、
夜间合成、逐行从左至右状态和阴影准备，仅最终量化位深不同。
EC 沿用旧版第四个红外输入仍为 K 的公式，没有在迁移中修正算法。
RTG 保留先在原网格非线性校正、后插值到高分辨率辅助网格的顺序。
通用 `ProductDefinition`、输入目标网格和输出帧网格支持这些依赖；MERSI/MWRI 同步使用 `load_product`。

正常 FY4 AGRI/GHI `render` 已切到 `native_disk`，失败不调用旧入口。
完整画幅使用规则网格面积采样，直接分配最终 framebuffer，没有整幅 RGB 临时图、投影顶点或 bins。
地理裁剪保留完整产品统计及行前缀，之后并行查找原生包围盒，以借用视图和局部投影顶点绘制。
经度周期声明避免跨投影断点的四边形污染画面。规则网格按着色后的面积累计，
新增独立密集面积参考、无效孔洞、带跨度视图及线程/块尺寸一致性测试。

FY4 默认原生主体尺寸及缩放锚点保留，完整 RTEC16 右边距为基准 100 像素，其他完整画幅为 300；
裁剪版式沿用扫描带的基准边距。原生行列标签使用整数；标题、页脚和色条与旧标签对照。
16 位文字使用最终尺寸的 8 位抗锯齿图层合成，经纬网以独立覆盖率图层合成，
不会把 16 位数值底图降为 8 位。16 位输出接受 PNG/TIFF，JPEG 明确报错。

### 产品及成图证据

`fy4_products_test` 通过旧模式函数生成无损参考，验证 AGRI A/B、GHI、模式别名及辅助文件的
47 个组合；GHI 目前使用合成文件，未声称完成真实 GHI 验收。
`fy4_cli_test` 验证正常入口、别名、1/4/自动线程、8/16 位、版式、原生经纬网、地理裁剪、
完整依赖计数、失败报告及缓存清理。Release 全套 18/18、独立核心 ASan/UBSan 9/9、
产品/适配器/两组 CLI 的 ASan/UBSan 6/6 全部通过；LSP MCP 对六个相关 C++ 文件诊断为 0 错误、0 警告。

真实 FY4B 4KM 的 E、R、Rr、RF、RY、RX、RTpp 最大颜色量化差为 0，EC、RTEC、RTEC16 为 1。
命令和输出位于 `.work/refactor-v2/native-reference/fy4-products-20261018/commands.json`。
真实 1KM 主数据配 4KM 辅助数据的 RTEC 对照有 92,194,660 个共同有效像元，最大量化差为 1。
三份实际输入已重新核对 SHA-256 并匹配冻结清单。

原生 CLI 产物位于 `.work/refactor-v2/native-reference/fy4-render-20261018/`，
包含 1KM 辅助 RTEC（822 × 824）、4KM E13（887 × 894）、4KM RTEC16（829 × 894）
和 RTEC16 区域裁剪（863 × 867），已目视检查。
`final-commands.json` 中六项均成功，包含最终 16 位 PNG/TIFF 解码完全一致、1/4 线程解码完全一致的检查。
这些是功能与质量诊断，没有预热一次加五次的正式新旧性能结论；正式测量仍在阶段 6。

## 阶段 4：PMR/GIIRS

### PMR 原生产品及入口

`sensors::identify_hdf_adapter` 在串行 I/O 通道内识别元数据及实际结构；PMR 必须具有
`SLV/zFactorCorrectedESurface` 及同文件或独立 GEO 的 `Geo_Fields/Latitude/Longitude`。
缺少 `Sensor Identification Code` 不再使正式入口在产品读取前失败，模式名称本身不作为识别依据。
PMR 以 `pmr` 注册；末轴为通道的三维定位只读取第 0 通道，也接受二维定位。
原始 `FillValue`/`_FillValue`/`Fill_Value` 先参与掩膜，再执行 Slope/Intercept 定标，定位与数值掩膜独立。
支持实际 PMR 文件的 `Valid_Range` 属性，空的可选范围不作为有效范围。

`products/pmr.hpp` 的产品定义及正式 CLI 接入通用扫描带路径，输出 dBZ 反射率，
保持区域包围盒、裁剪后有限定标值的最大值拉伸、`color_ir_ca`、`Long_Name` 及原始时间标签。
有限填充值仍可按显式声明参与旧统计口径，但不恢复掩膜。原始扫描列索引保留，
直接以最终尺寸面积栅格化，性能报告范围为 `native_swath`。

`pmr_products_test` 直接调用未修改的旧 PMR 函数，合成全景、5×5 局部区域及独立 GEO
在定标值、共同有效定位、投影前颜色上最大差异均为 0，标签和显示范围完全一致。
另检查填充值、二维/三维定位、窗口读取、区域外报错及 HDF5 对象释放，
包含 PMR 载荷名称变体、全零统计范围及全无效区域；读取样本计数确认三维定位仅读取第 0 通道。
`pmr_cli_test` 检查缺少/带有载荷属性、独立 GEO、掩膜、经纬网、正交投影、异常报告及清理；
1、4、自动线程的解码像素一致。最终 Release 全套 20/20、独立核心 ASan/UBSan 9/9，
HDF/FY4 适配器与 PMR 产品/CLI 的 ASan/UBSan 4/4 均通过；
LSP MCP 对六个相关 C++ 文件诊断为 0 错误、0 警告。
合成预览、复现命令、输入及可执行文件哈希保存在
`.work/refactor-v2/native-reference/pmr-synthetic-20261018/`，包含主体宽 800 的 1/4 线程解码一致检查。

### 真实 PMR 验收

NFS 重新挂载后，使用冻结的 FY3G PMR 2026-07-30 01:23 L2 文件，重新核对大小和 SHA-256，
与冻结清单一致。在 16°N、165°E、半径 10° 的原有负载上，区域包围盒共有 35,990 个样本，
定标值及定位差异为 0；8,371 个共同有效样本的投影前颜色量化差为 0，显示范围与标签一致。
无效填充值和超出实际有效范围的样本不绘制，有限值仍保留原统计口径，不能把覆盖变化称为提速。
最终原生 CLI 的 PMR PNG 为 863×955，1/4 线程解码像素完全一致，已目视检查。
证据、输入/可执行文件哈希及命令位于 `.work/refactor-v2/native-reference/pmr-real-20261018/`，
最终证据使用 `final-commands.json`、`final-manifest.json`、`input-validation-final.json`。

### GIIRS 原生读取与产品

`giirs` 适配器描述 VIS、LW、MW 三种网格。实际红外布局为 725/965 个光谱通道×128 个观测，
定位是长度 128 的展开序列，保持旧 Pixel/Line 属性所确定的 8×16 布局。
按窗口跨行步长选取单个通道的观测，不把光谱轴误作图像轴，也不读取整个辐亮度矩阵。
波数使用自己的 Slope/Intercept 定标，标签保留波数及 `cm^-1`；定位沿用旧直接读取的度数。

可见光保持 `DN²*c0 + DN*c1 + c2`，三个逐像元系数和 DN 分别按原属性定标。
真实系数表的 `valid_range=-1..1` 与第三项数百量级的系数不一致，且各项量纲不同，
因此不将表级范围作为各系数的观测有效范围；原始 FillValue 仍参与独立掩膜。
LW/MW 单通道沿用旧直接读取的原始辐亮度，IRall 则先做 Slope/Intercept 定标，
随后均用对应波数做普朗克反变换，输出 K，未改为摄氏温度。
产品依赖用 `raw_samples` 明确这一区别，核心双缓冲读取支持显式定标覆盖，有效性检查仍执行。

### IRall 与验证

`giirs_sequence_plan` 明确帧列表及每族统计，`stream_giirs_sequence` 逐通道计算一次，
把浮点数值和掩膜写入任务私有缓存，确定整个光谱族的极值后顺序消费。
每族仅保留一帧数值及共享定位，成图复用一次投影；MW、LW 各自采用共同色阶，先统计再裁剪。
保持 MW 在前、LW 在后及从 `000000` 起的六位连续编号，修复旧 IRall 的切片终点越界。
全无效单帧可在其光谱族仍有有限值时输出空白；全族没有有限值则报错，缓存仍清理。
默认 JPEG 命名及显式 PNG 目录/文件形式已验证，规则见 README。

`giirs_products_test` 用未修改的旧单通道函数及独立二维整族数值参考进行比较，
覆盖非单位 Slope/Intercept、逐像元系数、表级范围、填充值、展开窗口、独立定位有效性、
全无效帧/光谱族、波数异常、帧顺序、每族共同极值、读取次数及消费失败清理。
五帧合成负载的每通道读取一次；缓存总写入/读取各 600 字节，测试还约束帧场景缓冲峰值，
防止一次保留全部光谱帧。`giirs_cli_test` 覆盖 R/LW/MW/IRall、1/4/自动线程、独立 GEO、
裁剪、经纬网、失败报告及临时目录清理，三种线程配置解码像素一致。
合成五帧及可执行文件哈希位于 `.work/refactor-v2/native-reference/giirs-synthetic-20261018/`。

真实 FY4B GIIRS 文件已重新核对 SHA-256 并匹配冻结清单。
R 的 262,144 个像元、LW200 和 MW100 各 128 个观测的定标值、定位与投影前颜色最大差异均为 0，
显示范围及标签一致。原生 R（863×778）、LW200（863×1550）、MW100（863×1548）
及 LW200 裁剪（863×903）的 1/4 线程解码像素一致；已查看 R、LW200 及合成 IRall 帧。
真实证据位于 `.work/refactor-v2/native-reference/giirs-real-20261018/` 的最终命令、清单及输入哈希文件。
真实文件用于单通道验收，IRall 的多帧验收使用合成光谱文件，没有声称生成真实全部光谱帧。

最终 Release 全套 22/22、独立核心 ASan/UBSan 9/9、PMR/GIIRS 产品与 CLI 及 HDF/FY4 适配器
ASan/UBSan 6/6 均通过；LSP MCP 对八个相关 C++ 文件诊断为 0 错误、0 警告。
据此通过阶段 4，下一步为阶段 5 的全部产品回归整理及删除旧入口、bridge、TLS capture 和编译依赖。
本阶段证据是功能与质量验证，没有预热一次加五次的正式性能结论；阶段 6 仍待执行。

## 阶段 5：删除旧执行路径

### 独立产品参考

删除前的完整源码、十个验证程序及 SHA-256 已保存在
`.work/refactor-v2/checkpoint-stage4-20261018/`，阶段 4 的真实数据验证程序可以继续追溯。
以阶段 4 的测试驱动和合成 HDF 输入，改为链接冻结提交 `aaf6eef` 的头文件与
`libfy_pipeline.a` 导出旧实现的预期结果。原生实现只用于驱动中的对照断言，不提供预期值。
GIIRS IRall 使用旧二维读取和普朗克公式的独立整族参考，未执行已知越界切片。

得到 `tests/reference_v1/` 中 99 份记录，逻辑大小 574,103 字节：MERSI/MWRI 29、
FY4 47、PMR 4、GIIRS 单帧 7、整族光谱 12。保留可用的物理值、定位、标签、显示尺度与颜色；
FY4 保留 8/16 位量化颜色和标签。文件使用显式小端编码，不依赖拥有者的内存布局。
新增集合、大小及 SHA-256 校验，全部产品测试现在直接比对冻结数据，不编译旧函数。
生成驱动、源文件与编译/链接命令位于 `.work/refactor-v2/freeze-stage5-20261018/`。

### 正式构建与接口清理

删除 `src/legacy.cpp`、`src/mersi.cpp`、`src/pipeline.cpp`、整个旧头文件集合、bridge、
旧必需 RGB 栅格/帧及 TLS 捕获；移除 argv 回转和未识别载荷的旧实现回退。
`include/pipeline.hpp` 只保留标准库请求参数，不再暴露 OpenCV 图像类型或旧 Scene。
`fy_pipeline` 由 `fy_render` 替代，仅包含原生成图及检查命令，HDF5 与 OpenCV 保持实现依赖。

`inspect` 改为调用串行 I/O 通道中的传感器元数据接口，缺少载荷名称属性的实际 PMR 结构也可识别。
投影回归改用新核心，日间校正拆成可直接验证的标准库产品操作 `correct_agri_day`，
原有颜色断言保持不变。旧缩放测试由 `native_layout_test` 替代，验证两种宽度下的最终画布、
标题/页脚、经纬网隔离及 PMR/GIIRS/FY4 检查命令；正交与位深覆盖继续由各 CLI 测试承担。

冻结 reader 导出器不再由正式 CMake 构建，`build_frozen_sensor_reference.py` 先核对原始源归档哈希，
再在新目录构建独立旧 reader；不会修改基线归档。其验证程序显式接收 `--reference`。
新的冻结基线脚本默认钉住旧提交，不将重构后的 HEAD 误作旧实现。
已独立构建并用合成 MERSI E24 对照：2,048 个物理值误差为 0。

### 验收与边界

全新目录 `.work/refactor-v2/build-native-clean/` 完整 Release 构建及 CTest 23/23 通过，
完整原生 ASan/UBSan 23/23、独立核心 ASan/UBSan 9/9 均通过。
LSP MCP 对十个相关 C++ 文件诊断为 0 错误、0 警告。
`check_native_architecture.py` 检查 46 个源码/公共头文件、42 个编译单元及最终可执行文件符号，
旧执行依赖与旧执行符号均为 0，核心/产品层未引入应用库类型，HDF5 对象仅在传感器实现中出现。

使用清理前阶段 4 检查点和清理后全新构建，在相同真实输入及参数下比较：
MERSI E24、MWRI W8、FY4 RTEC16、PMR、GIIRS LW200、三段 MERSI RT 拼接，
六项 PNG 的编码字节完全一致，包含 16 位输出。
命令、二进制哈希、图像哈希及架构检查位于 `.work/refactor-v2/stage5-acceptance-20261018/`。
这些是清理前后原生路径的功能回归，不是阶段 0 旧算法与新算法的性能测量。

据此通过阶段 5。仍须执行阶段 6 的合成新载荷接入验收、正式功能/质量审计及
固定负载预热一次、测量五次的 1/4/自动线程性能测量，不能据本阶段标记整次重构完成。

## 阶段 6：正式验收

### 扩展性与通用成图

建立标准库产品注册表 `products::ProductRegistry`，与 `AdapterRegistry` 配对。
`render_registered_scene` 统一完成产品计划、区域策略、读取与计算；
`render_product` 按已有几何能力绘制，版式、标注、经纬网和编码移入 `src/render_product.cpp`。
区域载荷的默认半径由 `ProductPlan` 显式声明。FY4 兼容校验、GIIRS 光谱流及拼接输入约束
保留在编排入口，未进入通用 renderer/rasterizer。

`new_sensor_test` 注册 TEST-SAT/NOVEL-INSTRUMENT，使用独立 73 号波段、线性定标和 test-unit，
通过正式注册入口生成扫描带 RGB8（276×175）和规则图像 RGB16（323×386）。
1、4、自动线程像素完全一致，读取、查询及销毁都在同一个非主线程的串行 I/O 通道，
每次一个窗口、一次定位、一次产品计算；填充值保持无效，最终显式缓冲和缓存归零。
添加测试载荷时通用渲染文件哈希未变。随后统一修复 RGB16 TIFF 的 Adobe Deflate 编码参数，
并清理直接头文件依赖；这些修改对全部载荷通用，证据保留原始哈希及独立调整说明。

最终 Release 24/24、原生 ASan/UBSan 24/24、独立核心 ASan/UBSan 9/9 通过；
八个相关文件的 LSP MCP 诊断为 0 错误、0 警告。架构检查覆盖 51 个源码/头文件、45 个
编译单元及正式程序符号，旧依赖和符号均为 0。六项真实数据 PNG 与阶段 5 输出逐字节一致，
包括 MERSI E24、MWRI W8、FY4 RTEC16、PMR、GIIRS LW200 和三景拼接。
真实 RTEC16 TIFF 的标签确认 16 位及 compression=8。
证据位于 `.work/refactor-v2/stage6-architecture-20261018/`。

### 功能与质量审计对应关系

| 契约 | 独立证据 |
|---|---|
| 全部既有模式、RT16/RT+EC 别名、单位/标签/波段/定标 | 四组产品测试和 99 份冻结记录；阶段 2～4 的真实数值对照 |
| FillValue、有效范围、全无效、定位独立有效性 | 核心定标/模型/重采样、适配器和产品测试 |
| 阴影 halo、RTEC 行前缀、整景统计与区域依赖 | swath/FY4 产品测试、流式/重采样测试、真实裁剪对照 |
| 日期线、投影背面、细窄结构、孔洞及重叠裁决 | core_render_test 的解析/独立密集面积参考；真实正交拼接 |
| 线程与 tile 一致性、借用生命周期、异常清理 | 核心 raster/stream/cache、各 CLI 和 ASan/UBSan |
| 最终版式、grid 隔离、8/16 位无损输出 | native_layout_test、FY4/各载荷 CLI、六项真实 PNG 字节对照 |
| IRall 顺序、共同色阶、只计算一次及有界缓存 | 合成五帧产品/CLI；真实 GIIRS 单波段数值与成图 |

真实完整 1690 通道 IRall 未作为本轮负载，按计划使用合成光谱验证多帧语义。

### 正式测量

`tools/benchmark_refactor_v2.py` 重新核对冻结输入哈希，固定复制新旧可执行文件及新源码归档，
在同一个本地任务目录依次测量九项负载，每项旧版默认与新版 1/4/自动线程分别预热一次、测量五次。
测量输出与任务缓存不写入 NFS。旧版 PMR 入口失败单独记录，不能作为产品速度基线。
`tools/report_refactor_v2.py` 已独立核对完整 216 次运行（36 次预热、180 次测量）、参数、
输出/二进制哈希、位深、线程一致性和缓冲清理，生成成本明细及正式报告。
九项原生负载的每项 18 次输出均具有相同 SHA-256，1/4/自动线程与重复运行一致。
全部预期运行及输出检查通过；旧版 PMR 的六次 exit 2 是预先记录的入口失败，不是产品性能基线。

本次八项可与旧版成功成图比较的负载，自动线程墙钟均更长；例如 MERSI 1KM E24
由 1.030 秒增至 1.346 秒、250M E24 由 9.676 秒增至 13.095 秒、三景拼接由 3.453 秒
增至 9.231 秒，FY4 1KM RTEC 由 8.314 秒增至 67.202 秒。
MERSI 的定位有效性、完整画幅和采样工作变化单独列出，不计为算法提速；
其中裁剪画幅的边距取整相差一个像素，也已在报告中列出。
FY4 的窗口读取/重采样与 MERSI 面积栅格化是当前报告中的热点，具体底层原因仍待专门定位。
按计划不以 RSS 或提速比例设硬门槛，本轮通过迁移、正确性、质量与架构验收，
不将这一结果表述为全面性能提升。

原始命令、固定二进制、源码归档、全部图像/日志、逐次 profile、metrics、summary、details、
comparison 与 audit 位于 `/private/tmp/pg-ev-benchmark-v2-20261018/`。
完整测量目录归档为 `.work/refactor-v2/stage6-benchmark-20261018.tar`，附独立归档哈希清单。
真实 RTEC16 PNG/TIFF 最终解码逐像素一致、保留 uint16 三通道，差异为 0。
据此完成阶段 6；本轮计划阶段 0～6 均已完成，后续性能优化应使用新版本和独立测量目录。
