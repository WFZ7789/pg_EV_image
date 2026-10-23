# Metop-SG METimage L1B

支持 Metop-SG A 系列 VII/METimage L1B RAD 产品。NetCDF4 文件通过 HDF5 原生读取，
正式程序无需 NetCDF 库。`inspect`、`render` 和拼接任务的 `data` 都可直接传入 `.nc` 或
官方标准 ZIP32 压缩包；程序内部使用 ZLIB 解码 Stored/Deflate 成员，检查长度和 CRC，
将唯一的 `.nc` 放入任务临时目录，任务结束自动清理。无需手动解压。

本次依据 [EPS-SG VII Level 1B Product Format Specification v5A（2024-09-20）](https://user.eumetsat.int/s3/eup-strapi-media/EPS_SG_VII_Level_1_B_Product_Format_Specification_654c0b397a.pdf)
及 `testdata` 中的真实样本实现。本地 PDF 在 `~/Documents/METimage/`。
实际比例、偏移、有效范围、填充值和转换参数均读取文件，不把规范表内的示例定标值写死。

## 命令与通道

```bash
./cmake-build-release/pg_EV_image inspect scene.zip
./cmake-build-release/pg_EV_image render -d scene.zip -m RT --width 2400 -o output/metimage-RT.jpg
./cmake-build-release/pg_EV_image render -d scene.nc -m E -b 17 --width 2400 -o output/metimage-10690.png
./cmake-build-release/pg_EV_image render -d night.zip -m ECT --width 2400 -o output/metimage-night.png
```

定位、角度和定标包含在主文件内，不提供独立 `--geo` 或 `--aux`。
`inspect` 列出平台、完整观测时间、模式及从 0 开始的通道编号。

| 编号 | 波长 μm | 编号 | 波长 μm |
|---|---|---|---|
| 0 | 0.443 | 10 | 2.250 |
| 1 | 0.555 | 11 | 3.740 |
| 2 | 0.668 | 12 | 3.959 |
| 3 | 0.752 | 13 | 4.050 |
| 4 | 0.763 | 14 | 6.725 |
| 5 | 0.865 | 15 | 7.325 |
| 6 | 0.914 | 16 | 8.540 |
| 7 | 1.240 | 17 | 10.690 |
| 8 | 1.375 | 18 | 12.020 |
| 9 | 1.630 | 19 | 13.345 |

`R`、`RMUL` 接受太阳波段 `0..10`；`E` 接受热红外波段 `11..19`。
`R` 显示太阳角校正反射率，`RMUL` 使用现有单通道彩色拉伸，`E` 显示摄氏亮温并保留色条。
所有 20 个通道均可单独成图。

| 模式 | 输入及对应关系 |
|---|---|
| RT | RGB：0.668 / 0.555 / 0.443 μm，逐像元分子瑞利校正 |
| RY | RGB：1.630 / 0.865 / 0.668 μm，沿用现有假彩公式 |
| RF | 0.668、0.865 μm 反射率及 10.690 μm 亮温，沿用现有背景比值公式 |
| EC | 3.740、10.690、12.020 μm，12.020 μm 同时作为参考与冷云混合通道 |
| ECT | 3.740、8.540、10.690、12.020 μm，保留现有等亮度夜景、雾与卷云色相分支 |
| TSG | 现有定位诊断图 |

`EK` 已停止支持，METimage 不提供此模式。

`RT` 默认使用 gamma 0.5；`RT -c gamma05|linear|srgb|contrast`、其他模式的既有色表参数继续可用。
瑞利校正采用真实的 0.443/0.555/0.668 μm 名义波长，是现有分子路径散射近似；
未做逐探元光谱响应积分或气溶胶反演。夜间太阳波段保留原始缺测状态，各模式继续走同一处理链。
ECT 不插值填补原始缺测；冷云或无雾着色的分支不要求未参与计算的 3.74 μm 通道有效。

## 定标、质量与扫描定位

DN 在原始打包域检查 `_FillValue`、`valid_min/max`，再计算 `L = DN × scale_factor + add_offset`。
太阳波段以文件中的太阳辐照度 `E` 和日地距离比 `d` 得到 `πLd²/E`，产品阶段再除以太阳天顶角余弦。
热红外按文件中的中心波长执行普朗克逆变换，再计算 `T = A × TPlanck + B`；
公式内波长转为 m，辐亮度由每 μm 转为每 m，输出定标单位为 K，E 模式最后转为 ℃。
每个文件分别使用自己的转换参数。

质量标志按零起始位编号处理：

- `data_quality` 的缺测 L0、定标失败、定位失败位（0/2/4）屏蔽对应扫描和通道。
- `vii_processing_flag` 的缺测/损坏 L0 位屏蔽；饱和通过原始有效范围排除，太阳耀斑提示保留。
- 太阳波段定标质量中的不可用旧系数和缺少太阳几何/日地距离位（4/5）屏蔽；仍可用的降级系数保留。
- 定位质量中的无效编码器、轨道姿态、视线、地形及已报告视线误差位（0/1/2/4/6）屏蔽定位。
  太阳角异常位另外作用于太阳角度平面。
- `pixel_performance_mask` 的退化探元屏蔽；`pixel_duplication_mask=1` 丢弃，`0/2` 保留。

定位本身的有效性独立于数值重复掩膜，避免重复像元的剔除破坏相邻有效像元的地理四角。
无效物理数值为 NaN，不参与 METimage 的拉伸统计。

每次扫描有 24 行像元、独立的四行定位点，横向定位间隔为 8 像元。
按各扫描自身的定位点，在 WGS84 笛卡尔坐标内双线性插值，再恢复经纬度。
天顶角线性展开，方位角通过正弦/余弦插值处理 360° 周期。
DEM 修正采用规范的固定半径 `r=6371000 m`：纬度加 `δN/r`，经度加 `δE/(r cos φ)`，
随后统一经度表示。

投影和数值缓存以独立扫描条带处理，扫描边界不共用推导顶点。
阴影采样在 500 m 名义采样网格使用 2 像元步距，读取 halo 为 20 像元，并限制在同一扫描内。

## 连续轨段拼接

```bash
./cmake-build-release/pg_EV_image mosaic --job examples/metimage-mosaic.json --threads 4 --profile
```

任务沿用 version 1 JSON；`inputs` 至少两项，每项只需要 `data`，ZIP 和 NC 可以混用。
相对路径以任务 JSON 的目录为基准。样例使用 `testdata` 中两个相邻的真实 ZIP。
同一任务要求卫星、轨道和扫描宽度一致，按文件中的实际观测起始时间排序。

各段先独立定标，再把物理数值、角度和定位组织到任务级网格，统一准备产品。
白平衡、百分位、背景参数及夜间雾基线均从整项任务计算；数值和投影缓存保留扫描边界。
重叠采用现有扫描中心优先规则，平局按固定观测顺序决定。没有观测覆盖的位置保持空白。
标题保留平台和完整起止时段。

单景和拼接均支持 `--projection orthographic`、`--center-lat`、`--center-lon`、`--half-range`、
`--grid` 和 `--grid-step`；拼接的地图参数写入现有 `map` 字段。
输出 PNG/JPEG/TIFF，单景报告范围为 `native_swath`，拼接为 `native_mosaic`。

## 验证

默认 CTest 增加 `metimage_test` 和 `metimage_cli_test`，覆盖通道和定标、质量标志、
周期方位角、扫描子窗、缓存顶点、全部模式、ZIP/NC 等价、20 通道、昼夜红外、
拼接输入排序、线程一致性、区域、投影、编码和临时目录清理。

真实数据不作为默认 CTest 依赖。独立核验使用 Python/NumPy 和本机 NetCDF C 库，
不调用正式实现的定标、掩膜或插值辅助函数：

```bash
python3 tools/validate_metimage_samples.py \
  --app cmake-build-release/pg_EV_image --reader cmake-build-release/metimage_test
```

脚本对全部样本的扫描边界、中心和边缘窗口核对 20 通道、四组角度和 DEM 修正定位，
随后生成昼夜单通道、合成、连续轨段拼接、裁剪、投影、编码和线程一致性对照。
图像、完整输入摘要、误差、耗时、RSS、读取量和输出 SHA256 保存在 `output/metimage-validation/`，
`results.json` 为结构化结果，`README.md` 为拼接图索引。测试产物与真实数据不加入版本库。
