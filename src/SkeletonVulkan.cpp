/*
    SkeletonVulkan.cpp

    A skeleton After Effects plugin demonstrating Vulkan compute shader
    integration via the "hybrid rendering" pattern:

        1. Check out input pixels on CPU (PF_CHECKOUT_PARAM)
        2. Upload to GPU via Vulkan staging buffers
        3. Run a compute shader on the GPU
        4. Download results back to CPU memory
        5. Return CPU output to AE

    This approach works independently of AE's built-in GPU API, giving you
    full control over the Vulkan pipeline. It gracefully falls back to CPU
    rendering when Vulkan is unavailable.

    The effect itself is deliberately simple (brightness gain) so the GPU
    plumbing is easy to follow. Replace the compute shader with your own
    to build real effects.
*/

#include "SkeletonVulkan.h"

#ifdef HAVE_VULKAN
#include "gpu/VulkanRenderer.h"
#endif

// ===========================================
// Global state
// ===========================================

#ifdef HAVE_VULKAN
static VulkanRenderer* g_vulkanRenderer = nullptr;
#endif

// ===========================================
// About
// ===========================================

static PF_Err
About(
    PF_InData       *in_data,
    PF_OutData      *out_data,
    PF_ParamDef     *params[],
    PF_LayerDef     *output)
{
    AEGP_SuiteHandler suites(in_data->pica_basicP);

    suites.ANSICallbacksSuite1()->sprintf(
        out_data->return_msg,
        "%s v%d.%d\r%s",
        STR(StrID_Name),
        MAJOR_VERSION,
        MINOR_VERSION,
        STR(StrID_Description));

    return PF_Err_NONE;
}

// ===========================================
// GlobalSetup - one-time initialization
// ===========================================

static PF_Err
GlobalSetup(
    PF_InData       *in_data,
    PF_OutData      *out_data,
    PF_ParamDef     *params[],
    PF_LayerDef     *output)
{
    out_data->my_version = PF_VERSION(
        MAJOR_VERSION,
        MINOR_VERSION,
        BUG_VERSION,
        STAGE_VERSION,
        BUILD_VERSION);

    // SmartFX flags: we support SmartRender and deep color
    out_data->out_flags =
        PF_OutFlag_DEEP_COLOR_AWARE |
        PF_OutFlag_I_DO_DIALOG;         // Remove if you don't need a custom UI

    out_data->out_flags2 =
        PF_OutFlag2_SUPPORTS_SMART_RENDER |
        PF_OutFlag2_FLOAT_COLOR_AWARE;

    // Initialize Vulkan renderer
#ifdef HAVE_VULKAN
    g_vulkanRenderer = new VulkanRenderer();
    PF_Err vk_err = g_vulkanRenderer->Initialize();
    if (vk_err != PF_Err_NONE) {
        // Vulkan init failed - fall back to CPU. Not a fatal error.
        delete g_vulkanRenderer;
        g_vulkanRenderer = nullptr;
    }
#endif

    return PF_Err_NONE;
}

// ===========================================
// GlobalSetdown - cleanup
// ===========================================

static PF_Err
GlobalSetdown(
    PF_InData       *in_data,
    PF_OutData      *out_data,
    PF_ParamDef     *params[],
    PF_LayerDef     *output)
{
#ifdef HAVE_VULKAN
    if (g_vulkanRenderer) {
        g_vulkanRenderer->Shutdown();
        delete g_vulkanRenderer;
        g_vulkanRenderer = nullptr;
    }
#endif

    return PF_Err_NONE;
}

// ===========================================
// ParamsSetup - define effect parameters
// ===========================================

static PF_Err
ParamsSetup(
    PF_InData       *in_data,
    PF_OutData      *out_data,
    PF_ParamDef     *params[],
    PF_LayerDef     *output)
{
    PF_Err      err = PF_Err_NONE;
    PF_ParamDef def;

    // Gain slider (0-100, default 10)
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX(
        STR(StrID_Gain_Param_Name),
        SKELETON_GAIN_MIN,
        SKELETON_GAIN_MAX,
        SKELETON_GAIN_MIN,
        SKELETON_GAIN_MAX,
        SKELETON_GAIN_DFLT,
        PF_Precision_HUNDREDTHS,
        0,
        0,
        GAIN_DISK_ID);

    // Color picker
    AEFX_CLR_STRUCT(def);
    PF_ADD_COLOR(
        STR(StrID_Color_Param_Name),
        PF_HALF_CHAN8,
        PF_MAX_CHAN8,
        PF_MAX_CHAN8,
        COLOR_DISK_ID);

    // GPU mode dropdown
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP(
        STR(StrID_GPUMode_Param_Name),
        2,                              // num choices
        GPU_MODE_GPU,                   // default: GPU
        STR(StrID_GPUMode_Choices),     // "CPU|GPU"
        USE_GPU_DISK_ID);

    out_data->num_params = SKELETON_NUM_PARAMS;

    return err;
}

// ===========================================
// CPU pixel processing (fallback)
// ===========================================

static PF_Err
GainFunc8(
    void        *refcon,
    A_long      xL,
    A_long      yL,
    PF_Pixel8   *inP,
    PF_Pixel8   *outP)
{
    PreRenderData *dataP = reinterpret_cast<PreRenderData*>(refcon);
    if (!dataP) return PF_Err_NONE;

    PF_FpLong gain = dataP->gainF * PF_MAX_CHAN8 / 100.0;
    if (gain > PF_MAX_CHAN8) gain = PF_MAX_CHAN8;

    outP->alpha = inP->alpha;
    outP->red   = MIN((A_long)(inP->red   + gain), PF_MAX_CHAN8);
    outP->green = MIN((A_long)(inP->green + gain), PF_MAX_CHAN8);
    outP->blue  = MIN((A_long)(inP->blue  + gain), PF_MAX_CHAN8);

    return PF_Err_NONE;
}

static PF_Err
GainFunc16(
    void        *refcon,
    A_long      xL,
    A_long      yL,
    PF_Pixel16  *inP,
    PF_Pixel16  *outP)
{
    PreRenderData *dataP = reinterpret_cast<PreRenderData*>(refcon);
    if (!dataP) return PF_Err_NONE;

    PF_FpLong gain = dataP->gainF * PF_MAX_CHAN16 / 100.0;
    if (gain > PF_MAX_CHAN16) gain = PF_MAX_CHAN16;

    outP->alpha = inP->alpha;
    outP->red   = MIN((A_long)(inP->red   + gain), PF_MAX_CHAN16);
    outP->green = MIN((A_long)(inP->green + gain), PF_MAX_CHAN16);
    outP->blue  = MIN((A_long)(inP->blue  + gain), PF_MAX_CHAN16);

    return PF_Err_NONE;
}

static PF_Err
GainFuncFloat(
    void            *refcon,
    A_long          xL,
    A_long          yL,
    PF_PixelFloat   *inP,
    PF_PixelFloat   *outP)
{
    PreRenderData *dataP = reinterpret_cast<PreRenderData*>(refcon);
    if (!dataP) return PF_Err_NONE;

    PF_FpLong gain = dataP->gainF / 100.0;

    outP->alpha = inP->alpha;
    outP->red   = inP->red   + (PF_FpShort)gain;
    outP->green = inP->green + (PF_FpShort)gain;
    outP->blue  = inP->blue  + (PF_FpShort)gain;

    return PF_Err_NONE;
}

// ===========================================
// SmartRender: PreRender
// ===========================================

static PF_Err
PreRender(
    PF_InData           *in_data,
    PF_OutData          *out_data,
    PF_PreRenderExtra   *extra)
{
    PF_Err err = PF_Err_NONE;

    // Allocate pre-render data to pass to SmartRender
    PF_RenderRequest req = extra->input->output_request;
    PF_CheckoutResult result;

    // Tell AE we need the full input
    ERR(extra->cb->checkout_layer(
        in_data->effect_ref,
        SKELETON_INPUT,
        SKELETON_INPUT,
        &req,
        in_data->current_time,
        in_data->time_step,
        in_data->time_scale,
        &result));

    // Store our pre-render data
    if (!err) {
        // Set output rect to match input
        UnionLRect(&result.result_rect, &extra->output->result_rect);
        UnionLRect(&result.max_result_rect, &extra->output->max_result_rect);
    }

    return err;
}

// ===========================================
// SmartRender: Render
// ===========================================

static PF_Err
SmartRender(
    PF_InData           *in_data,
    PF_OutData          *out_data,
    PF_SmartRenderExtra *extra)
{
    PF_Err err = PF_Err_NONE;
    AEGP_SuiteHandler suites(in_data->pica_basicP);

    // Checkout input and output layers
    PF_EffectWorld *input_worldP = nullptr;
    PF_EffectWorld *output_worldP = nullptr;

    ERR(extra->cb->checkout_layer_pixels(in_data->effect_ref, SKELETON_INPUT, &input_worldP));
    ERR(extra->cb->checkout_output(in_data->effect_ref, &output_worldP));

    if (err || !input_worldP || !output_worldP) return err;

    // Read parameter values
    PF_ParamDef param_gain, param_color, param_gpu;
    AEFX_CLR_STRUCT(param_gain);
    AEFX_CLR_STRUCT(param_color);
    AEFX_CLR_STRUCT(param_gpu);

    ERR(PF_CHECKOUT_PARAM(
        in_data, SKELETON_GAIN,
        in_data->current_time,
        in_data->time_step,
        in_data->time_scale,
        &param_gain));

    ERR(PF_CHECKOUT_PARAM(
        in_data, SKELETON_COLOR,
        in_data->current_time,
        in_data->time_step,
        in_data->time_scale,
        &param_color));

    ERR(PF_CHECKOUT_PARAM(
        in_data, SKELETON_GPU_MODE,
        in_data->current_time,
        in_data->time_step,
        in_data->time_scale,
        &param_gpu));

    if (err) return err;

    PreRenderData renderData;
    AEFX_CLR_STRUCT(renderData);
    renderData.gainF    = param_gain.u.fs_d.value;
    renderData.color    = param_color.u.cd.value;
    renderData.gpu_mode = param_gpu.u.pd.value;

    // Detect pixel format using WorldSuite2
    // PF_WORLD_IS_DEEP can't distinguish 16-bit from 32-bit float
    int pixelDepth = 8;
    PF_WorldSuite2 *wsP = nullptr;
    if (in_data->pica_basicP->AcquireSuite(
            kPFWorldSuite, kPFWorldSuiteVersion2,
            (const void**)&wsP) == kSPNoError && wsP) {
        PF_PixelFormat pf = PF_PixelFormat_INVALID;
        wsP->PF_GetPixelFormat(output_worldP, &pf);
        if (pf == PF_PixelFormat_ARGB128) {
            pixelDepth = 32;
        } else if (pf == PF_PixelFormat_ARGB64) {
            pixelDepth = 16;
        }
        in_data->pica_basicP->ReleaseSuite(kPFWorldSuite, kPFWorldSuiteVersion2);
    } else if (PF_WORLD_IS_DEEP(output_worldP)) {
        // Fallback heuristic: check bytes per pixel from rowbytes
        pixelDepth = 16;
    }

    // Try GPU path first
    bool rendered_on_gpu = false;

#ifdef HAVE_VULKAN
    if (renderData.gpu_mode == GPU_MODE_GPU && g_vulkanRenderer && g_vulkanRenderer->IsAvailable()) {
        AEPixelFormat fmt;
        switch (pixelDepth) {
            case 32: fmt = AEPixelFormat::ARGB32F; break;
            case 16: fmt = AEPixelFormat::ARGB16;  break;
            default: fmt = AEPixelFormat::ARGB8;   break;
        }

        PF_Err gpu_err = g_vulkanRenderer->RenderGain(
            input_worldP,
            output_worldP,
            renderData.gainF,
            fmt);

        if (gpu_err == PF_Err_NONE) {
            rendered_on_gpu = true;
        }
        // If GPU render failed, fall through to CPU
    }
#endif

    // CPU fallback
    if (!rendered_on_gpu) {
        A_long linesL = output_worldP->extent_hint.bottom - output_worldP->extent_hint.top;

        if (pixelDepth == 32) {
            ERR(suites.IterateFloatSuite1()->iterate(
                in_data,
                0,
                linesL,
                input_worldP,
                NULL,
                (void*)&renderData,
                GainFuncFloat,
                output_worldP));
        } else if (pixelDepth == 16) {
            ERR(suites.Iterate16Suite2()->iterate(
                in_data,
                0,
                linesL,
                input_worldP,
                NULL,
                (void*)&renderData,
                GainFunc16,
                output_worldP));
        } else {
            ERR(suites.Iterate8Suite2()->iterate(
                in_data,
                0,
                linesL,
                input_worldP,
                NULL,
                (void*)&renderData,
                GainFunc8,
                output_worldP));
        }
    }

    // Check in parameters
    ERR(PF_CHECKIN_PARAM(in_data, &param_gain));
    ERR(PF_CHECKIN_PARAM(in_data, &param_color));
    ERR(PF_CHECKIN_PARAM(in_data, &param_gpu));

    return err;
}

// ===========================================
// Legacy Render (non-SmartFX fallback)
// ===========================================

static PF_Err
Render(
    PF_InData       *in_data,
    PF_OutData      *out_data,
    PF_ParamDef     *params[],
    PF_LayerDef     *output)
{
    PF_Err err = PF_Err_NONE;
    AEGP_SuiteHandler suites(in_data->pica_basicP);

    PreRenderData renderData;
    AEFX_CLR_STRUCT(renderData);
    A_long linesL = output->extent_hint.bottom - output->extent_hint.top;
    renderData.gainF = params[SKELETON_GAIN]->u.fs_d.value;

    if (PF_WORLD_IS_DEEP(output)) {
        ERR(suites.Iterate16Suite2()->iterate(
            in_data, 0, linesL,
            &params[SKELETON_INPUT]->u.ld,
            NULL,
            (void*)&renderData,
            GainFunc16,
            output));
    } else {
        ERR(suites.Iterate8Suite2()->iterate(
            in_data, 0, linesL,
            &params[SKELETON_INPUT]->u.ld,
            NULL,
            (void*)&renderData,
            GainFunc8,
            output));
    }

    return err;
}

// ===========================================
// Plugin registration (modern entry point)
// ===========================================

extern "C" DllExport
PF_Err PluginDataEntryFunction2(
    PF_PluginDataPtr inPtr,
    PF_PluginDataCB2 inPluginDataCallBackPtr,
    SPBasicSuite* inSPBasicSuitePtr,
    const char* inHostName,
    const char* inHostVersion)
{
    PF_Err result = PF_REGISTER_EFFECT_EXT2(
        inPtr,
        inPluginDataCallBackPtr,
        "Skeleton Vulkan",          // Name
        "ADBE SkeletonVulkan",      // Match Name
        "Sample Plug-ins",          // Category
        AE_RESERVED_INFO,
        "EffectMain",               // Entry point
        "https://github.com/anthropics/claude-code");

    return result;
}

// ===========================================
// Main entry point - command dispatcher
// ===========================================

PF_Err
EffectMain(
    PF_Cmd          cmd,
    PF_InData       *in_data,
    PF_OutData      *out_data,
    PF_ParamDef     *params[],
    PF_LayerDef     *output,
    void            *extra)
{
    PF_Err err = PF_Err_NONE;

    try {
        switch (cmd) {
            case PF_Cmd_ABOUT:
                err = About(in_data, out_data, params, output);
                break;

            case PF_Cmd_GLOBAL_SETUP:
                err = GlobalSetup(in_data, out_data, params, output);
                break;

            case PF_Cmd_GLOBAL_SETDOWN:
                err = GlobalSetdown(in_data, out_data, params, output);
                break;

            case PF_Cmd_PARAMS_SETUP:
                err = ParamsSetup(in_data, out_data, params, output);
                break;

            case PF_Cmd_RENDER:
                err = Render(in_data, out_data, params, output);
                break;

            case PF_Cmd_SMART_PRE_RENDER:
                err = PreRender(in_data, out_data,
                    reinterpret_cast<PF_PreRenderExtra*>(extra));
                break;

            case PF_Cmd_SMART_RENDER:
                err = SmartRender(in_data, out_data,
                    reinterpret_cast<PF_SmartRenderExtra*>(extra));
                break;
        }
    }
    catch (PF_Err &thrown_err) {
        err = thrown_err;
    }

    return err;
}
