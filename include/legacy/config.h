#ifndef PG_EV_IMAGE_CONFIG_H
#define PG_EV_IMAGE_CONFIG_H

#include "stl.h"

/// @brief 渲染配置结构体，用于替代全局变量，支持多线程与多实例渲染
struct RenderConfig {
    float lowval = 0.0f;
    float hival = 1.0f;
};

/// @brief 卫星与传感器配置表
struct SatConfig {
    std::string sat_name;

    struct BandConfig {
        std::string akdataset;
        unsigned akid;
        std::string qkdataset;
        unsigned transid;
        double mersi_equiv_mid_wn;
    };

    std::map<int, BandConfig> bands;
};

inline std::string get_viscal_name(const std::string& sat_name) {
    if (sat_name == "FY-3G")
        return "RSB_Cal_Coeff";
    return "VIS_Cal_Coeff";
}

/// @brief 预定义的 FY-3 系列卫星红外通道参数
inline std::map<std::string, SatConfig> kSatConfigs = {
    {
        "FY-3D",
        {
            "FY-3D",
            {
                {
                    1, {"EV_250_Aggr.1KM_RefSB", 0, "EV_250_RefSB_b1", 0, 0.0}
                },
                {
                    2, {"EV_250_Aggr.1KM_RefSB", 1, "EV_250_RefSB_b2", 1, 0.0}
                },
                {
                    3, {"EV_250_Aggr.1KM_RefSB", 2, "EV_250_RefSB_b3", 2, 0.0}
                },
                {
                    4, {"EV_250_Aggr.1KM_RefSB", 3, "EV_250_RefSB_b4", 3, 0.0}
                },
                {
                    5, {"EV_1KM_RefSB", 0, "", 4, 0.0}
                },
                {
                    6, {"EV_1KM_RefSB", 1, "", 5, 0.0}
                },
                {
                    7, {"EV_1KM_RefSB", 2, "", 6, 0.0}
                },
                {
                    8, {"EV_1KM_RefSB", 3, "", 7, 0.0}
                },
                {
                    9, {"EV_1KM_RefSB", 4, "", 8, 0.0}
                },
                {
                    10, {"EV_1KM_RefSB", 5, "", 9, 0.0}
                },
                {
                    11, {"EV_1KM_RefSB", 6, "", 10, 0.0}
                },
                {
                    12, {"EV_1KM_RefSB", 7, "", 11, 0.0}
                },
                {
                    13, {"EV_1KM_RefSB", 8, "", 12, 0.0}
                },
                {
                    14, {"EV_1KM_RefSB", 9, "", 13, 0.0}
                },
                {
                    15, {"EV_1KM_RefSB", 10, "", 14, 0.0}
                },
                {
                    16, {"EV_1KM_RefSB", 11, "", 15, 0.0}
                },
                {
                    17, {"EV_1KM_RefSB", 12, "", 16, 0.0}
                },
                {
                    18, {"EV_1KM_RefSB", 13, "", 17, 0.0}
                },
                {
                    19, {"EV_1KM_RefSB", 14, "", 18, 0.0}
                },
                {
                    20, {"EV_1KM_Emissive", 0, "", 0, 2634.359}
                },
                {
                    21, {"EV_1KM_Emissive", 1, "", 1, 2471.654}
                },
                {
                    22, {"EV_1KM_Emissive", 2, "", 2, 1382.621}
                },
                {
                    23, {"EV_1KM_Emissive", 3, "", 3, 1168.182}
                },
                {
                    24, {"EV_250_Aggr.1KM_Emissive", 0, "EV_250_Emissive_b24", 4, 933.364}
                },
                {
                    25, {"EV_250_Aggr.1KM_Emissive", 1, "EV_250_Emissive_b25", 5, 836.941}
                }
            }
        }
    },
    {
        "FY-3E",
        {
            "FY-3E",
            {
                {
                    1, {"EV_1KM_LL", 0, "", 0, 0.0}
                },
                {
                    2, {"EV_1KM_Emissive", 0, "", 0, 2623.369}
                },
                {
                    3, {"EV_1KM_Emissive", 1, "", 1, 2466.214}
                },
                {
                    4, {"EV_1KM_Emissive", 2, "", 2, 1384.461}
                },
                {
                    5, {"EV_1KM_Emissive", 3, "", 3, 1164.837}
                },
                {
                    6, {"EV_250_Aggr.1KM_Emissive", 0, "EV_250_Emissive_b24", 4, 926.606}
                },
                {
                    7, {"EV_250_Aggr.1KM_Emissive", 1, "EV_250_Emissive_b25", 5, 837.013}
                }
            }
        }
    },
    {
        "FY-3F",
        {
            "FY-3F",
            {
                {
                    1, {"EV_250_Aggr.1KM_RefSB", 0, "EV_250_RefSB_b1", 0, 0.0}
                },
                {
                    2, {"EV_250_Aggr.1KM_RefSB", 1, "EV_250_RefSB_b2", 1, 0.0}
                },
                {
                    3, {"EV_250_Aggr.1KM_RefSB", 2, "EV_250_RefSB_b3", 2, 0.0}
                },
                {
                    4, {"EV_250_Aggr.1KM_RefSB", 3, "EV_250_RefSB_b4", 3, 0.0}
                },
                {
                    5, {"EV_1KM_RefSB", 0, "", 4, 0.0}
                },
                {
                    6, {"EV_1KM_RefSB", 1, "", 5, 0.0}
                },
                {
                    7, {"EV_1KM_RefSB", 2, "", 6, 0.0}
                },
                {
                    8, {"EV_1KM_RefSB", 3, "", 7, 0.0}
                },
                {
                    9, {"EV_1KM_RefSB", 4, "", 8, 0.0}
                },
                {
                    10, {"EV_1KM_RefSB", 5, "", 9, 0.0}
                },
                {
                    11, {"EV_1KM_RefSB", 6, "", 10, 0.0}
                },
                {
                    12, {"EV_1KM_RefSB", 7, "", 11, 0.0}
                },
                {
                    13, {"EV_1KM_RefSB", 8, "", 12, 0.0}
                },
                {
                    14, {"EV_1KM_RefSB", 9, "", 13, 0.0}
                },
                {
                    15, {"EV_1KM_RefSB", 10, "", 14, 0.0}
                },
                {
                    16, {"EV_1KM_RefSB", 11, "", 15, 0.0}
                },
                {
                    17, {"EV_1KM_RefSB", 12, "", 16, 0.0}
                },
                {
                    18, {"EV_1KM_RefSB", 13, "", 17, 0.0}
                },
                {
                    19, {"EV_1KM_RefSB", 14, "", 18, 0.0}
                },
                {
                    20, {"EV_1KM_Emissive", 0, "", 0, 2641.6461}
                },
                {
                    21, {"EV_1KM_Emissive", 1, "", 1, 2476.5705}
                },
                {
                    22, {"EV_1KM_Emissive", 2, "", 2, 1384.8775}
                },
                {
                    23, {"EV_1KM_Emissive", 3, "", 3, 1165.6627}
                },
                {
                    24, {"EV_250_Aggr.1KM_Emissive", 0, "EV_250_Emissive_b24", 4, 937.37106}
                },
                {
                    25, {"EV_250_Aggr.1KM_Emissive", 1, "EV_250_Emissive_b25", 5, 828.08198}
                }
            }
        }
    },
    {
        "FY-3G",
        {
            "FY-3G",
            {
                {
                    1, {"EV_Reflectance", 0, "", 0, 0.0}
                },
                {
                    2, {"EV_Reflectance", 1, "", 1, 0.0}
                },
                {
                    3, {"EV_Reflectance", 2, "", 2, 0.0}
                },
                {
                    4, {"EV_Reflectance", 3, "", 3, 0.0}
                },
                {
                    5, {"EV_Reflectance", 4, "", 4, 0.0}
                },
                {
                    6, {"EV_Emissive", 0, "", 0, 2624.158}
                },
                {
                    7, {"EV_Emissive", 1, "", 1, 929.837}
                },
                {
                    8, {"EV_Emissive", 2, "", 2, 830.676}
                }
            }
        }
    },
    {
        "FY-3H",
        {
            "FY-3H",
            {
                {
                    1, {"EV_250_Aggr.1KM_RefSB", 0, "EV_250_RefSB_b1", 0, 0.0}
                },
                {
                    2, {"EV_250_Aggr.1KM_RefSB", 1, "EV_250_RefSB_b2", 1, 0.0}
                },
                {
                    3, {"EV_250_Aggr.1KM_RefSB", 2, "EV_250_RefSB_b3", 2, 0.0}
                },
                {
                    4, {"EV_250_Aggr.1KM_RefSB", 3, "EV_250_RefSB_b4", 3, 0.0}
                },
                {
                    5, {"EV_1KM_RefSB", 0, "", 4, 0.0}
                },
                {
                    6, {"EV_1KM_RefSB", 1, "", 5, 0.0}
                },
                {
                    7, {"EV_1KM_RefSB", 2, "", 6, 0.0}
                },
                {
                    8, {"EV_1KM_RefSB", 3, "", 7, 0.0}
                },
                {
                    9, {"EV_1KM_RefSB", 4, "", 8, 0.0}
                },
                {
                    10, {"EV_1KM_RefSB", 5, "", 9, 0.0}
                },
                {
                    11, {"EV_1KM_RefSB", 6, "", 10, 0.0}
                },
                {
                    12, {"EV_1KM_RefSB", 7, "", 11, 0.0}
                },
                {
                    13, {"EV_1KM_RefSB", 8, "", 12, 0.0}
                },
                {
                    14, {"EV_1KM_RefSB", 9, "", 13, 0.0}
                },
                {
                    15, {"EV_1KM_RefSB", 10, "", 14, 0.0}
                },
                {
                    16, {"EV_1KM_RefSB", 11, "", 15, 0.0}
                },
                {
                    17, {"EV_1KM_RefSB", 12, "", 16, 0.0}
                },
                {
                    18, {"EV_1KM_RefSB", 13, "", 17, 0.0}
                },
                {
                    19, {"EV_1KM_RefSB", 14, "", 18, 0.0}
                },
                {
                    20, {"EV_1KM_Emissive", 0, "", 0, 2634.322236}
                },
                {
                    21, {"EV_1KM_Emissive", 1, "", 1, 2468.717622}
                },
                {
                    22, {"EV_1KM_Emissive", 2, "", 2, 1385.940533}
                },
                {
                    23, {"EV_1KM_Emissive", 3, "", 3, 1169.168006}
                },
                {
                    24, {"EV_250_Aggr.1KM_Emissive", 0, "EV_250_Emissive_b24", 4, 931.601394}
                },
                {
                    25, {"EV_250_Aggr.1KM_Emissive", 1, "EV_250_Emissive_b25", 5, 835.885240}
                }
            }
        }
    }
};

#endif
