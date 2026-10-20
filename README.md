# pg_EV_image

风云卫星 HDF5 图像渲染工具，支持 FY3 MERSI、MWRI、PMR 和 FY4 AGRI/GHI、GIIRS 的现有产品模式，并提供 MERSI 经纬网与连续轨段拼接。

## 构建

依赖 CMake 4、C++23、HDF5 C/C++、OpenCV、TBB、CLI11 和 ZLIB。

```bash
cmake --preset release
cmake --build --preset release -j 4
ctest --test-dir cmake-build-release --output-on-failure
```

仅构建 CPU 核心与测试可使用 `-DFY_BUILD_APP=OFF`，只需 oneTBB 和系统线程库。
如果已有构建目录使用不同生成器，请通过 `cmake -S . -B 新目录 -G Ninja` 建立独立目录。

## 使用

查看卫星、载荷和文件别名：

```bash
./cmake-build-release/pg_EV_image inspect 数据.HDF
```

渲染单景 FY3 MERSI 亮温图：

```bash
./cmake-build-release/pg_EV_image render \
  -m E -b 24 -d 数据.HDF -g 定位.HDF -o output/亮温.png
```

FY3 MERSI 的 `EC` 可用 1KM 文件作为主数据，并通过 `--aux` 附加同星、同观测时间的
250M 文件。此时 10.8/12.0 μm 波段读取原生 250M 数据，其余 EC 输入和 GEO1K
插值到 250M 网格：

```bash
./cmake-build-release/pg_EV_image render \
  -m EC -d 1000M数据.HDF -g GEO1K定位.HDF -a 0250M数据.HDF \
  -o output/ec_250m.png
```

`ECT` 为月光夜景试验模式：按 10.8 μm 亮温在 Oklab 插值的 9 节点色带上取色，保留 EC 的
太阳阴影；低云/雾与薄卷云只旋转色相、等亮度混合，因此亮度仍然只对应温度。雾判据只在
太阳天顶角大于约 90° 时生效，卷云判据只在 FY-3D、FY-3E 上启用。

```bash
./cmake-build-release/pg_EV_image render \
  -m ECT -d 数据.HDF -g 定位.HDF -o output/moon.png
```

指定区域和正交投影：

```bash
./cmake-build-release/pg_EV_image render \
  -m E -b 24 -d 数据.HDF -g 定位.HDF \
  --projection orthographic --center-lat 35 --center-lon 140 \
  --half-range 20 --width 2400 --grid --grid-step 5 -o output/区域.png
```

拼接相邻 MERSI 轨段：复制 [示例任务](examples/mersi-mosaic.json)，为每段填写真实的数据和 GEO 路径，然后运行：

```bash
./cmake-build-release/pg_EV_image mosaic --job 任务.json
```

`render` 和 `mosaic` 均支持 `--threads 4`（0 为自动）、`--profile` 和
`--scratch-dir /本地缓存父目录`。报告路径是在输出参数后附加 `.profile.json`；
任务临时目录结束时自动清理。MERSI/MWRI 单景使用原生窗口读取和目标尺寸栅格器，
报告范围为 `native_swath`；MERSI 拼接使用任务数值缓存、共同统计和并行栅格器，
报告范围为 `native_mosaic`。FY4 AGRI/GHI 使用原生产品和最终尺寸成图，报告范围为
`native_disk`。PMR 根据 `Geo_Fields/SLV` 结构识别，缺少载荷名称属性也可通过原生路径成图，
报告范围为 `native_swath`。GIIRS 使用原生光谱路径，报告范围为 `native_spectrum`；
IRall 按光谱族缓存数值、确定共同色阶后逐帧输出。
新核心的接口和阶段验证见 [重构执行记录](docs/REFACTOR_V2_PROGRESS.md)。
正式程序与默认测试均已移除 legacy 编译依赖和旧 CLI 回转。
产品回归使用[冻结参考记录](tests/reference_v1/README.md)，独立旧版本导出器的使用方法见
[参考工具说明](tools/REFERENCE_WORKFLOWS.md)。
正式功能、架构及九项负载测量已完成，详见[性能与成本报告](docs/REFACTOR_V2_PERFORMANCE.md)。
本次八项可与旧版成功成图比较的负载，原生自动线程墙钟均更长；FY4 1KM RTEC 的
中位数由约 8.31 秒增至 67.20 秒。迁移和采样质量验证通过，当前版本存在明确的性能回退。

任务中 `inputs` 至少两组。单次任务要求同一卫星、载荷和分辨率；产品与波段在任务级统一设置。程序按观测时间排序，重叠处选靠近扫描中心的有效像元。亮温 `E` 以及真彩/假彩 `RT`、`RY`、`RMUL` 模式使用整个任务的共同显示范围。时间间隔超过十分钟会提示，未覆盖区域保持空白。

FY3 单景保留产品公式、标题、页脚、坐标及色条锚点；直接按主体宽度创建画布，
使用面积覆盖采样，不以旧图缩放后的逐像素相等作为验收要求。无效 GEO 不参与画幅计算。
单景主体宽度按裁剪后的行数自动确定（至少 3840 像素，完整 250M 轨道通常为 12800 像素）；
`--width` 直接设置最终主体宽度，文字、边距和色条按版式比例绘制。
标题从 HDF 读取卫星、仪器和观测时间，保留秒及 UTC。默认不画经纬网；使用 `--grid`
在主体地图内叠加网格，间距自动确定，也可用 `--grid-step` 指定。
grid 不改变成图尺寸、裁剪、标题、页脚或色条。拼接任务用 `map.grid: 1` 开启。
输出按扩展名选择 PNG、JPEG 或 TIFF。
输出目录不存在时会创建；失败会返回非零状态。FY4 完整画幅默认保留原始主体尺寸，
`--width` 可直接指定最终主体宽度；具有完整导航的 FY4B 全圆盘支持经纬度区域裁剪。
FY4 的 `RTEC16`／`RT16` 使用 16 位 PNG 或 TIFF；JPEG 不支持该位深。
FY4 经纬网需要圆盘导航，正交投影当前仍用于 MERSI 地图流程。

## 现有产品

MERSI：`E`、`R`、`LL`、`RT`、`RY`、`RF`、`RMUL`、`EC`、`ECT`、`EK`、`TSG`。
FY4 AGRI/GHI：`E`、`R`、`Re`、`Rr`、`EC`、`RF`、`RY`、`RX`、`RTpp`、`RTEC`、`RTEC16`、`RTG`、`AGHI_ALD`。
GIIRS：`R`、`LW`、`MW`、`IRall`。MWRI：`S`、`W`、`W84`、`TCX`、`TCN`、`TCN2`、`TCS`、`TCR`。降水产品：`PMR`。

GIIRS 的 `LW`、`MW` 使用从 0 开始的光谱通道编号，页脚保留中心波数，亮温单位为 K。
`IRall` 先输出全部 MW 通道，再输出全部 LW 通道，以 `000000` 起连续编号；每个光谱族共用显示范围，
裁剪不改变统计范围。默认输出 `output/000000.jpg` 等文件。
指定 `-o output/光谱/序列.png` 时，在该父目录输出 `000000.png` 等文件；
指定无扩展名路径 `-o output/光谱` 时将其作为目录，输出 PNG。

单图命令从旧版 `-m E -d 数据.HDF -g 定位.HDF -b 24 -o 名称` 改为 `render -m E -d 数据.HDF -g 定位.HDF -b 24 -o output/名称.png`。旧版 `--aq` 改为 `--projection orthographic`，`-x/-y/-r` 改为 `--center-lat/--center-lon/--half-range`。产品含义保持原样。

代码结构与新增产品方法见 [架构说明](docs/ARCHITECTURE.md)。

## 许可证

开发贡献：Codex。

本项目自有代码采用 [MIT License](LICENSE)，包括
`include/`、`src/`、`tests/` 下的代码、构建配置和根目录的 Python/Shell 脚本。
第三方组件及其素材保留各自许可证，不因本项目采用 MIT 而重新授权。

允许商用、修改、再分发及闭源使用；复制或分发本软件或其重要部分时，须保留
版权和许可声明。MIT 不要求公开修改后的源码。卫星输入数据和生成图像的使用权
不由此代码许可证授予。

依赖许可、核对范围和二进制分发注意事项见 [许可核对记录](docs/LICENSING.md)。
