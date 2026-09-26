# 许可核对记录

核对日期：2026-09-27。项目自有代码采用 MIT，全文见根目录 `LICENSE`。
C++ 文件使用 `SPDX-License-Identifier: MIT` 通知；构建配置和 Python/Shell 脚本的
许可范围由 README 与此文件声明。许可证署名按项目维护者要求写为 `Codex`；
该署名及 Git 作者字段不作为版权归属核验依据。未对第三方组件重新授权。

## 核对结论与范围

检查了 Git 跟踪文件、CMake 的直接依赖、本机依赖许可证、Release 构建缓存和
可执行文件的直接动态链接。未发现已有项目许可证、第三方源码版权通知或明确的
GPL/AGPL 源码引入。已核对的依赖许可允许与 MIT 自有代码组合，未发现阻止本项目
自有代码采用 MIT 的条款。Apache/BSD 等依赖仍按原许可证分发，不能统一替换成 MIT。

这不证明未标注出处的旧代码全部原创。Git 作者字段也不是版权归属证明；若有外部
复制代码、共同作者或职务作品，应先确认相应授权，并保留原有通知。此记录不是
全部递归依赖或未来二进制发行包的完整许可审计。

## 已核对组件

| 组件 | 本机版本或来源 | 许可证 | 处理方式 |
|---|---|---|---|
| OpenCV | 5.0.0 | Apache-2.0；内含组件另有许可 | 保留 LICENSE、COPYRIGHT 和所用第三方组件通知 |
| HDF5 C/C++ | 2.2.0 | BSD-3-Clause，含附加版权及资助声明 | 保留该版本完整 LICENSE，而不只复制通用 BSD 模板 |
| oneTBB | 2023.1.0 | Apache-2.0 | 保留许可证及适用的通知 |
| CLI11 | 2.7.2，头文件库 | BSD-3-Clause | 发布编译后的程序也应随附其版权、条件与免责声明 |
| ZLIB | macOS SDK / 系统 libz | Zlib | 修改或再分发其源码时保留原通知，不误述来源 |
| libaec | HDF5 构建缓存中的依赖 | BSD-2-Clause | 随包分发时保留其完整许可通知 |
| Plasma 色表 | `include/legacy/color.h` 中的查找表 | 原始 BIDS Plasma 色表采用 CC0 | 原始色表可用于 MIT 项目；实际复制路径未有记录 |

本机许可文件来自 `/opt/homebrew/opt/{opencv,hdf5,tbb,cli11,libaec}/`。
当前 CMake 将 OpenCV 的多个模块链接进程序，不能只按 core/imgproc 两个模块
推断完整发行包的依赖范围。

## 分发要求

- 复制或分发本软件或其重要部分时，保留根目录 LICENSE 中的版权与许可声明。
  MIT 允许商业和闭源使用，不要求提供修改版源码。
- 随包提供 CLI11 的完整 BSD 通知，以及实际包含的其他依赖所需的许可证、版权和
  NOTICE。此核对表不能替代这些完整文本。
- 打包 OpenCV/HDF5 等动态库时，按实际版本核对递归依赖和启用的可选模块，收集
  图像编解码、视频、压缩及其他第三方组件的通知。本次未制作二进制发行包。
- 下载脚本调用系统 `wget`，仓库不包含 wget；如将其随发行包分发，需另外遵守其许可。
- 本项目许可证不授予卫星数据、外部素材或图像中第三方内容的使用权。

## 官方依据

- [MIT 原文（Open Source Initiative）](https://opensource.org/license/mit)
- [OpenCV 许可证](https://opencv.org/license/)
- [HDF5 许可证](https://github.com/HDFGroup/hdf5/blob/develop/LICENSE)
- [oneTBB 许可证](https://github.com/uxlfoundation/oneTBB/blob/master/LICENSE.txt)
- [CLI11 许可证](https://github.com/CLIUtils/CLI11/blob/main/LICENSE)
- [ZLIB 许可证](https://zlib.net/zlib_license.html)
- [libaec](https://github.com/Deutsches-Klimarechenzentrum/libaec)
- [BIDS 色表及 CC0 声明](https://bids.github.io/colormap/)

上游链接可能随版本变化；分发时应以实际包含版本的许可文本为准。
