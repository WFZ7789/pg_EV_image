/// SPDX-License-Identifier: MIT

#ifndef PG_EV_IMAGE_MODES_H
#define PG_EV_IMAGE_MODES_H

/// @brief 模式实现伞头：按载荷分组拆分子头文件，避免单文件过大
/// context.h        公共部分（Context 结构、太阳校正）
/// modes_pmr_mwri.h FY-3 降水产品 (PMR) 与微波成像仪 (MWRI) 模式
/// modes_mersi.h    FY-3 MERSI 普通模式 + FY-3 分发
/// modes_fy4.h      FY-4 静止轨道模式 + rayleigh 辅助 + FY-4 分发
/// modes_giirs.h    GIIRS 模式
#include "context.h"
#include "modes_pmr_mwri.h"
#include "modes_mersi.h"
#include "modes_fy4.h"
#include "modes_giirs.h"

#endif
