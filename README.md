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

指定区域和正交投影：

```bash
./cmake-build-release/pg_EV_image render \
  -m E -b 24 -d 数据.HDF -g 定位.HDF \
  --projection orthographic --center-lat 35 --center-lon 140 \
  --half-range 20 --width 2400 --grid-step 5 -o output/区域.png
```

拼接相邻 MERSI 轨段：复制 [示例任务](examples/mersi-mosaic.json)，为每段填写真实的数据和 GEO 路径，然后运行：

```bash
./cmake-build-release/pg_EV_image mosaic --job 任务.json
```

任务中 `inputs` 至少两组。单次任务要求同一卫星、载荷和分辨率；产品与波段在任务级统一设置。程序按观测时间排序，重叠处选靠近扫描中心的有效像元。亮温 `E` 以及真彩/假彩 `RT`、`RY`、`RMUL` 模式使用整个任务的共同显示范围。时间间隔超过十分钟会提示，未覆盖区域保持空白。

地图默认宽度为 3840 像素，自动确定经纬网间距；可用 `--no-grid` 关闭。输出按扩展名选择 PNG、JPEG 或 TIFF。输出目录不存在时会创建；失败会返回非零状态。FY4 原生圆盘和其他载荷继续使用原生渲染路径，正交投影与经纬网当前作用于 MERSI 地图流程。

## 现有产品

MERSI：`E`、`R`、`LL`、`RT`、`RY`、`RF`、`RMUL`、`EC`、`EK`、`TSG`。
FY4 AGRI/GHI：`E`、`R`、`Re`、`Rr`、`EC`、`RF`、`RY`、`RX`、`RTpp`、`RTEC`、`RTEC16`、`RTG`、`AGHI_ALD`。
GIIRS：`R`、`LW`、`MW`、`IRall`。MWRI：`S`、`W`、`W84`、`TCX`、`TCN`、`TCN2`、`TCS`、`TCR`。降水产品：`PMR`。

单图命令从旧版 `-m E -d 数据.HDF -g 定位.HDF -b 24 -o 名称` 改为 `render -m E -d 数据.HDF -g 定位.HDF -b 24 -o output/名称.png`。旧版 `--aq` 改为 `--projection orthographic`，`-x/-y/-r` 改为 `--center-lat/--center-lon/--half-range`。产品含义保持原样。

代码结构与新增产品方法见 [架构说明](docs/ARCHITECTURE.md)。
