#include "SkeletonVulkan.h"

typedef struct {
    A_u_long    index;
    A_char      str[256];
} TableString;

TableString g_strs[StrID_NUMTYPES] = {
    StrID_NONE,                 "",
    StrID_Name,                 "Skeleton Vulkan",
    StrID_Description,          "A skeleton AE plugin with Vulkan GPU compute.\rUse as a starting point for GPU-accelerated effects.",
    StrID_Gain_Param_Name,      "Gain",
    StrID_Color_Param_Name,     "Color",
    StrID_UseGPU_Param_Name,    "Use GPU",
};

char *GetStringPtr(int strNum)
{
    return g_strs[strNum].str;
}
