# pg_EV_image

风云卫星 HDF5 图像渲染工具，支持 FY3 MERSI、MWRI、PMR 和 FY4 AGRI/GHI、GIIRS 的现有产品模式，并提供 MERSI 经纬网与连续轨段拼接。

## 构建

依赖 CMake 4、C++23、HDF5 C/C++、OpenCV、TBB、CLI11 和 ZLIB。

```bash
cmake --preset release
cmake --build --preset release -j 4
ctest --test-dir cmake-build-release --output-on-failure
```

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

任务中 `inputs` 至少两组。单次任务要求同一卫星、载荷和分辨率；产品与波段在任务级统一设置。程序按观测时间排序，重叠处选靠近扫描中心的有效像元。亮温 `E` 以及真彩/假彩 `RT`、`RY`、`RMUL` 模式使用整个任务的共同显示范围。时间间隔超过十分钟会提示，未覆盖区域保持空白。

FY3 成图沿用 `legacy-baseline` 的尺寸、裁剪、投影、标题、页脚、坐标和色条格式。
单景主体宽度按裁剪后的行数自动确定（至少 3840 像素，完整 250M 轨道通常为 12800 像素）；
`--width` 将完整旧版画布等比缩放到指定的主体宽度，文字和色条一起缩放。
标题从 HDF 读取卫星、仪器和观测时间，保留秒及 UTC。默认不画经纬网；使用 `--grid`
在旧版主体地图内叠加网格，间距自动确定，也可用 `--grid-step` 指定。
grid 不改变成图尺寸、裁剪、标题、页脚或色条。拼接任务用 `map.grid: 1` 开启。
输出按扩展名选择 PNG、JPEG 或 TIFF。
输出目录不存在时会创建；失败会返回非零状态。FY4 原生圆盘和其他载荷继续使用原生渲染路径，
正交投影与经纬网当前作用于 MERSI 地图流程。

## 现有产品

MERSI：`E`、`R`、`LL`、`RT`、`RY`、`RF`、`RMUL`、`EC`、`EK`、`TSG`。
FY4 AGRI/GHI：`E`、`R`、`Re`、`Rr`、`EC`、`RF`、`RY`、`RX`、`RTpp`、`RTEC`、`RTEC16`、`RTG`、`AGHI_ALD`。
GIIRS：`R`、`LW`、`MW`、`IRall`。MWRI：`S`、`W`、`W84`、`TCX`、`TCN`、`TCN2`、`TCS`、`TCR`。降水产品：`PMR`。

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
