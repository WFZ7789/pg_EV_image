# 架构与扩展

第二次重构的基础接口位于 `include/core/`，实现位于 `src/core/`，由 `fy_core` 独立构建。
它包含只能移动的数值块、借用视图、几何描述、串行 I/O 适配会话、产品/显示/版式描述、
oneTBB 执行上下文及性能报告。使用 `-DFY_BUILD_APP=OFF` 可仅构建核心及其测试，
无需 HDF5、OpenCV 或 CLI11。`fy_sensors` 已提供 MERSI/MWRI、PMR、FY4 AGRI/GHI 和 GIIRS 原生窗口读取，
核心提供双缓冲定标、定位重采样、圆盘导航，以及扫描带和规则图像网格的面积覆盖栅格器。
AMSR3 L1B 通过独立的 `Amsr3Adapter` 接入同一注册表与 I/O 通道，直接读取 NetCDF4 的
HDF5 数据集；产品以 `amsr3` 注册，无需 NetCDF 库。详情见 [AMSR3 支持说明](AMSR3.md)。
METimage 以 `metimage` 注册，`MetimageAdapter` 同样直接读取 NetCDF4 的 HDF5 数据集；
`sensors/input.hpp` 使用 ZLIB 将 ZIP 中的 NetCDF 解码到任务 scratch，入口统一处理输入路径。
产品复用现有光学/红外计算与显示公式，通道映射、按波长定标和瑞利波长按 METimage 设置。
详见 [METimage 支持说明](METimage.md)。
`fy_products` 实现上述载荷的产品依赖与显示策略，`src/native_swath.cpp` 编排这些载荷的
单景 `render` 和 MERSI/METimage `mosaic`。`src/render_product.cpp` 按几何能力完成投影、最终尺寸
栅格化、版式与编码；读取和产品公式独立于这一通用入口。
`fy_render` 只包含原生成图与检查命令，应用通过 CLI11 接收请求。
HDF5 会话、入口探测与 `inspect` 元数据读取均在同一 I/O 通道执行，计算通过 oneTBB；
OpenCV 用于任务 JSON 读取、标注、经纬网和编码，不进入公共数值接口。
详见 [执行记录](REFACTOR_V2_PROGRESS.md)。
功能、架构与正式测量已完成；成本分解和已知性能回退见[性能报告](REFACTOR_V2_PERFORMANCE.md)。

程序入口位于 `src/app.cpp`：`inspect` 读取元数据，`render` 处理单景，`mosaic` 从 JSON
任务读取 MERSI 数据/GEO 配对或 METimage 主文件。JSON 相对路径以任务文件所在目录为基准。

`include/core/raster.hpp` 和 `include/core/model.hpp` 提供原生栅格与物理产品帧。
产品可依既有统计口径保留带无效掩膜的有限数值，但这些样本不能参与绘制；默认定标和
定位重采样仍将无效输入变为 NaN。MERSI 在裁剪前保留全景统计和阴影依赖，MWRI 在读取
区域包围盒后计算局部极值，PMR 保留局部最大值拉伸；源列号保留到栅格化，不因裁剪改变扫描中心的定义。
`include/pipeline.hpp` 的 `Request` 仍汇总产品与地图参数。
`include/products/cache.hpp` 将每景数值、掩膜与定位写入任务临时文件，正式绘制复用这些数据。
几何准备每次只加载一景；栅格器按块加载缓存，以 LRU 控制每个工作线程持有的源块数据量，
默认预算为 16 MiB。多个整景数值不会同时保留在内存中，缓存不存储 RGB。
拼接时保留原始地理定位网格，栅格化使用每个观测像元的地理四角；在重叠处选择更接近
扫描中心的有效像元，时间和输入路径决定平局顺序。
经纬网默认关闭；单景命令通过 `--grid`、拼接任务通过 `map.grid: 1` 开启。

全部已知载荷单景和 MERSI/METimage 拼接均经原生路径成图，错误直接返回。
旧 `src/mersi.cpp`、`src/pipeline.cpp`、`src/legacy.cpp`、bridge、必需 RGB 帧、TLS 捕获及
`include/legacy/` 已删除，正式目标和默认测试不编译旧路径。
`include/pipeline.hpp` 仅保留标准库任务参数和检查命令声明；`src/inspect.cpp` 经传感器层查询元数据。
投影测试改用 `core::MapProjection`，日间颜色测试改用 `correct_agri_day`，
旧整幅图缩放像素相等测试改为原生最终尺寸、版式、坐标/色条区域及经纬网隔离契约测试。

产品测试使用 `tests/reference_v1/` 的 99 份冻结数值/颜色记录，不在测试中编译或执行旧产品函数。
记录源于固定旧提交的头文件与静态库，校验器核对文件集合、尺寸与 SHA-256。
真实数据的独立 reader 工具只在显式提取的旧源码归档副本中构建；
与当前目标分离，方法见 [参考工具说明](../tools/REFERENCE_WORKFLOWS.md)。

PMR 的识别检查实际 `SLV/zFactorCorrectedESurface` 与 `Geo_Fields/Latitude/Longitude`，
允许载荷名称属性缺失，允许独立 GEO 文件。三维定位只读末轴第 0 通道，保留二维窗口形状；
按原始填充值确定掩膜，再执行定位及反射率定标。产品以区域包围盒的有限定标值确定显示上限，
保留原来的 dBZ、色表与标签；带无效掩膜的有限值可参与既有统计口径，但不参与绘制。

GIIRS 以 `giirs` 注册：红外数据的首轴为光谱通道，末轴为展开的观测序列，
按既有 Pixel/Line 属性顺序恢复观测网格；窗口读取用跨行步长选择所需观测，不加载光谱立方体。
VIS 使用原生二维定位与逐像元三项定标系数。系数的量纲不同，表级 `valid_range` 不作为各列的
观测有效范围，FillValue 仍独立屏蔽。保持旧公式 `DN²*c0 + DN*c1 + c2`。
`InputDependency::raw_samples` 显式声明 LW/MW 单通道使用未做线性变换的辐亮度；
IRall 使用描述符的 Slope/Intercept，二者均在计算线程进行普朗克反变换，保留 K 单位。

`products/giirs.hpp` 声明 IRall 帧列表及逐帧消费接口。每族只保留一帧数值，数值与掩膜写入
任务临时文件，累计完整光谱族的极值后顺序读取成图；每帧只读取、计算一次。
每族定位与投影复用，MW、LW 分别使用共同显示范围，统计先于地理裁剪。
修复旧 IRall 的切片终点越界，保留 MW 在前、LW 在后及六位连续编号。
报告范围为 `native_spectrum`，IRall 的 `source_pixels` 累计各帧观测数，`cache.scenes` 计数缓存帧数。

FY4 适配器以 `fy4` 注册，读取 NOM 原始 DN 窗口，CAL 查找表由核心计算线程定标；
原始 DN 填充值与定标输出填充值分别描述。太阳/卫星角度保留独立网格，供产品层对齐。
完整圆盘的导航参数来自 HDF 属性，窗口读取仅返回 `NativeDiskGeometry`，
`core/navigation.hpp` 通过椭球交点计算经纬度，并提供地理位置到原生行列的反算。
区域文件或缺少导航属性的文件使用 `ImageGeometry` 描述原生画幅，不隐含地理定位；
这些文件仍可按原生画幅成图，地理裁剪和经纬网则要求完整圆盘导航。

通用 `ProductDefinition` 及 `load_product` 按每个输入的目标网格对齐依赖，输出帧可指定独立网格。
FY4 的 `RTEC`／`RTEC16` 共用产品准备与逐行递推，仅最终量化不同。
RTG 在原网格完成非线性校正后再插值到辅助网格，太阳角从原始网格分别对齐，避免改变插值顺序。
完整圆盘直接对规则网格做面积采样，不建立扫描带 bins 或整幅投影顶点；
地理裁剪在完整产品准备后确定原生包围盒，再以借用视图和局部投影顶点绘制。
16 位标注使用最终尺寸的 8 位抗锯齿图层后合成，数值底图保留 16 位精度。

新增 MERSI 产品时，先定义输入波段、校正和产品值，返回同尺寸数值栅格与有效性；
然后在 `fy_products` 中注册模式。新载荷建立独立 `SensorAdapter`，通过 `AdapterRegistry`
注册读取工厂，通过 `products::ProductRegistry` 注册产品工厂。`render_registered_scene`
使用相同键取得两个工厂，按 `ProductPlan` 读取、定标、计算，再调用 `render_product`；
产品算法不执行文件输出。区域载荷的默认半径由产品计划声明，不由 renderer 检查载荷名称。

`InputDependency::geographic_alignment` 显式声明按实际观测位置对齐，优先于数组尺寸判断。
核心在单位球面的源网格三角形内生成权重，通过空间索引查找目标观测；不对覆盖外或无效
三角形外推。相同源网格的多个波段复用权重，`ProductLoadCache` 在同一传感器会话内复用
跨扫描组所需的源定位与物理数值，生命周期仅限该次任务。
`render_swath_products` 接收多个同产品扫描带，统一地图范围并复用既有栅格重叠策略，
不读取载荷名称。AMSR3 AB 的 A/B 选择和准备留在应用编排层，合并前不交错数组行或生成 RGB 缓存。
扫描带地图支持最高 360° 经度跨度，跨地图切口的源单元保持已有断开规则，避免横跨整图连线。

`include/render_product.hpp` 接收已准备的数值产品，按 `SwathGeometry`、`NativeDiskGeometry`
或 `ImageGeometry` 分派，renderer/rasterizer 不判断卫星或载荷名称。
`ObservationGeometry` 仍是未来几何能力接口，当前通用入口明确拒绝，未虚构观测足迹。
FY4 兼容限制与 GIIRS 多帧编排留在应用入口层；GIIRS 复用通用版式/编码和核心栅格器。

`new_sensor_test` 仅在测试中添加独立适配器、产品描述与注册，使用自己的载荷名称、73 号波段
和单位，经同一 `render_registered_scene` 入口生成扫描带 RGB8 与规则图像 RGB16。
它验证串行 I/O 生命周期、定标/无效值、完整读取次数、最终尺寸与位深，以及 1/4/自动线程
像素一致，未为测试载荷增加 renderer 分支。架构检查另核对正式编译依赖、符号、层次及
通用 renderer 中已知载荷名称字面量；冻结旧程序仅是离线验证工具。

拼接时 MERSI `E` 模式使用所有轨段的共同亮温范围；`RT`、`RY`、`RMUL` 使用共同的
99% 反射率拉伸。其他使用场景局部百分位拉伸的合成模式可拼接，但仍可能出现亮度接缝。
统计域沿用既有产品定义；共同统计在数值准备后完成，再应用于各景显示描述。
HDF5 读取、定标和产品准备每景执行一次，缓存块可以按需重复读取。
多景只接受文件名带 `YYYYMMDD_HHMM` 和分辨率的输入；
每份 GEO 必须显式配对。

旧版源码基线保存在 Git 标签 `legacy-baseline`。真实数据、任务文件和输出图片都被
`.gitignore` 排除，不应强制加入仓库。
