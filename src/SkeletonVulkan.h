#pragma once

#ifndef SKELETON_VULKAN_H
#define SKELETON_VULKAN_H

typedef unsigned char       u_char;
typedef unsigned short      u_short;
typedef unsigned short      u_int16;
typedef unsigned long       u_long;
typedef short int           int16;

#define PF_TABLE_BITS   12
#define PF_TABLE_SZ_16  4096

#define PF_DEEP_COLOR_AWARE 1   // Enable 16bpc pixels

#include "AEConfig.h"

#ifdef AE_OS_WIN
    typedef unsigned short PixelType;
    #include <Windows.h>
#endif

#include "entry.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_Macros.h"
#include "Param_Utils.h"
#include "AE_EffectCBSuites.h"
#include "String_Utils.h"
#include "AE_GeneralPlug.h"
#include "AEFX_ChannelDepthTpl.h"
#include "AEGP_SuiteHandler.h"
#include "Smart_Utils.h"

#include "SkeletonVulkan_Strings.h"

// Versioning
#define MAJOR_VERSION   1
#define MINOR_VERSION   0
#define BUG_VERSION     0
#define STAGE_VERSION   PF_Stage_DEVELOP
#define BUILD_VERSION   1

// Parameter defaults
#define SKELETON_GAIN_MIN   0
#define SKELETON_GAIN_MAX   100
#define SKELETON_GAIN_DFLT  10

// GPU mode dropdown values (1-indexed, AE popup convention)
enum {
    GPU_MODE_CPU = 1,
    GPU_MODE_GPU,
};

// Parameter indices
enum {
    SKELETON_INPUT = 0,
    SKELETON_GAIN,
    SKELETON_COLOR,
    SKELETON_GPU_MODE,      // Popup: CPU or GPU rendering
    SKELETON_NUM_PARAMS
};

// Disk IDs (for parameter persistence across versions)
enum {
    GAIN_DISK_ID = 1,
    COLOR_DISK_ID,
    USE_GPU_DISK_ID,
};

// Data passed from PreRender to SmartRender
typedef struct PreRenderData {
    PF_FpLong   gainF;
    PF_Pixel    color;
    A_long      gpu_mode;
} PreRenderData;

extern "C" {
    DllExport
    PF_Err
    EffectMain(
        PF_Cmd          cmd,
        PF_InData       *in_data,
        PF_OutData      *out_data,
        PF_ParamDef     *params[],
        PF_LayerDef     *output,
        void            *extra);
}

#endif // SKELETON_VULKAN_H
