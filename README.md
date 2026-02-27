# Skeleton Vulkan - After Effects Plugin Template

A minimal After Effects plugin skeleton that demonstrates **Vulkan compute shader** integration. Fork this and replace the shader to build your own GPU-accelerated effects.

## Why Vulkan in an AE Plugin?

After Effects provides its own GPU API (CUDA / OpenCL / Metal via `PF_Cmd_GPU_DEVICE_SETUP` and `PF_Cmd_SMART_RENDER_GPU`), but it has limitations:

- **No access to unmasked source layers** - AE's GPU checkout returns the masked result, which breaks effects that need the original pixels (e.g., projection, distortion, anything that resamples)
- **Limited control** - You're constrained to AE's GPU memory model and shader formats
- **Framework fragmentation** - You need separate implementations for CUDA (NVIDIA), OpenCL, and Metal

**Vulkan solves all of this:**

- **Full GPU control** - You own the Vulkan device, pipeline, and memory
- **Cross-platform** - Works on Windows (native) and macOS (via MoltenVK), on any GPU vendor
- **Unmasked source** - You checkout pixels on CPU via `PF_CHECKOUT_PARAM`, upload to GPU, process, and download back. AE never touches the GPU path, so masks don't interfere
- **Write once** - A single GLSL compute shader runs everywhere (compiled to SPIR-V)

The tradeoff is the upload/download overhead (CPU <-> GPU memory transfer), but for any non-trivial computation, the GPU speedup far exceeds this cost.

## Architecture: "Hybrid Rendering"

```
After Effects                          Your Plugin
     |                                      |
     |  PF_Cmd_SMART_RENDER                 |
     |------------------------------------->|
     |                                      |
     |  Checkout input (CPU memory)         |
     |  +----------------------------+      |
     |  | Upload to GPU via staging  |------+-> Vulkan Staging Buffer
     |  +----------------------------+      |        |
     |                                      |   VkImage (GPU)
     |                                      |        |
     |  +----------------------------+      |   Compute Shader
     |  | Dispatch compute shader    |------+-> (gain.comp)
     |  +----------------------------+      |        |
     |                                      |   VkImage (GPU)
     |                                      |        |
     |  +----------------------------+      |   Download via staging
     |  | Download result to CPU     |------+-> (HOST_CACHED for speed)
     |  +----------------------------+      |
     |                                      |
     |  Return output (CPU memory)          |
     |<-------------------------------------|
```

## Project Structure

```
AE_Skeleton_Vulkan/
  CMakeLists.txt                 # Build system (cross-platform)
  headers/
    AEConfig.h                   # AE platform detection
  src/
    SkeletonVulkan.h             # Main plugin header
    SkeletonVulkan.cpp           # Plugin entry point, SmartFX, CPU fallback
    SkeletonVulkan_Strings.h     # String table IDs
    SkeletonVulkan_Strings.cpp   # String table
    gpu/
      VulkanRenderer.h           # Vulkan renderer class
      VulkanRenderer.cpp         # Full Vulkan pipeline implementation
      gain_shader_spv.h          # Compiled SPIR-V (auto-generated)
  shaders/
    gain.comp                    # GLSL compute shader (source)
    gain.spv                     # Compiled SPIR-V binary
    compile_shader.bat           # Windows: recompile shader
  resources/
    SkeletonVulkanPiPL.r         # PiPL resource (macOS Rez format)
    SkeletonVulkanPiPL.rc        # PiPL resource (Windows)
```

## Prerequisites

1. **After Effects SDK** - Download from [Adobe](https://developer.adobe.com/after-effects/)
2. **Vulkan SDK** - Download from [LunarG](https://vulkan.lunarg.com/sdk/home)
3. **CMake 3.20+** - Download from [cmake.org](https://cmake.org/download/)
4. **Visual Studio 2019+** (Windows) or **Xcode** (macOS)

## Building

### Windows

```bash
# Set the SDK path (or place AfterEffectsSDK adjacent to this folder)
mkdir build && cd build
cmake -G "Visual Studio 17 2022" -A x64 -DAESDK_ROOT="C:/path/to/AfterEffectsSDK" ..
cmake --build . --config Release
```

Output: `build/Release/SkeletonVulkan.aex`

Copy the `.aex` to your AE plugins folder:
```
C:\Program Files\Adobe\Adobe After Effects <version>\Support Files\Plug-ins\
```

### macOS

```bash
mkdir build && cd build
cmake -DAESDK_ROOT="/path/to/AfterEffectsSDK" ..
cmake --build . --config Release
```

Output: `build/Release/SkeletonVulkan.plugin`

### Vulkan SDK Auto-Detection

CMake automatically finds the Vulkan SDK from:
- `VULKAN_SDK` environment variable (set by the SDK installer)
- `C:/VulkanSDK/*` (Windows, picks latest version)
- `~/VulkanSDK/*/macOS` (macOS, picks latest version)

If Vulkan is not found, the plugin **still builds** - it just won't have GPU rendering (CPU fallback only).

## Modifying the Shader

1. Edit `shaders/gain.comp` with your GLSL compute shader
2. Recompile:
   ```bash
   cd shaders
   compile_shader.bat   # Windows
   # OR manually:
   glslc gain.comp -o gain.spv
   xxd -i gain.spv > ../src/gpu/gain_shader_spv.h  # Linux/macOS
   ```
3. If you change the descriptor bindings or uniform buffer layout, update `VulkanRenderer.cpp` to match:
   - `CreateDescriptorSetLayout()` - binding types and counts
   - `UpdateDescriptorSet()` - what gets bound
   - `GainUniforms` struct - must match shader's `layout(std140)`
   - `RenderGain()` - upload new uniform data

## Key Implementation Details

### GPU Enable Dropdown

The **GPU Enable** parameter is a dropdown (popup) in the Effect Controls panel with two options:

| Choice | Behavior |
|--------|----------|
| **CPU** | Always renders on the CPU using AE's iterate suites. Use this for debugging, comparison, or systems without Vulkan. |
| **GPU** | Renders via the Vulkan compute shader. If Vulkan is unavailable or the GPU render fails, the plugin **automatically falls back to CPU** — you'll get a correct result either way. |

The default is **GPU**. The dropdown value is stored with the project, so it persists across saves and reopens.

**How the fallback works in code** (`SmartRender`):

1. If the dropdown is set to GPU **and** the Vulkan renderer initialized successfully, attempt GPU rendering
2. If the GPU render returns an error (e.g., device lost, out of memory), fall through to CPU
3. If the dropdown is set to CPU, skip the GPU path entirely

To extend the dropdown with additional modes (e.g., "GPU (Debug)"), add entries to the `GpuMode` enum in `SkeletonVulkan.h` and the `"CPU|GPU"` choices string in `SkeletonVulkan_Strings.cpp`.

### SmartFX Rendering

This plugin uses the SmartFX pattern (`PF_Cmd_SMART_PRE_RENDER` + `PF_Cmd_SMART_RENDER`) which is required for 32-bit float color and modern AE features. The legacy `PF_Cmd_RENDER` path is also provided as a fallback (CPU-only).

### Thread Safety (MFR)

After Effects uses Multi-Frame Rendering (MFR), meaning your render function may be called from multiple threads simultaneously. The Vulkan renderer handles this with:

- **Per-thread command pools** - Each render thread gets its own `VkCommandPool`, so command buffers can be built in parallel
- **Queue mutex** - Only the `vkQueueSubmit` call is serialized (this is fast)
- **Descriptor pool mutex** - Descriptor allocation is serialized

### Delay-Loading (Windows)

On Windows, `vulkan-1.dll` is delay-loaded (`/DELAYLOAD`). This means the plugin loads even if Vulkan drivers aren't installed - it just falls back to CPU rendering. Without delay-loading, a missing DLL would prevent the entire plugin from loading.

### HOST_CACHED Memory

GPU-to-CPU downloads use `VK_MEMORY_PROPERTY_HOST_CACHED_BIT` for dramatically faster CPU reads (~6x vs uncached `HOST_COHERENT`). This requires an explicit `vkInvalidateMappedMemoryRanges` call before reading, which the code handles automatically.

### PiPL Resource

The PiPL (Plugin Property List) tells AE about your plugin's capabilities. Key flags:

- `PF_OutFlag_DEEP_COLOR_AWARE` - Supports 16-bit color
- `PF_OutFlag2_SUPPORTS_SMART_RENDER` - Uses SmartFX rendering
- `PF_OutFlag2_FLOAT_COLOR_AWARE` - Supports 32-bit float color

On Windows, PiPL is normally preprocessed from `.r` format through the SDK's PiPLTool. The modern `PluginDataEntryFunction2` entry point handles registration directly in code, reducing PiPL complexity.

## What to Change for Your Plugin

1. **Plugin identity**: Update names in `SkeletonVulkan_Strings.cpp`, PiPL match name in both `.r` and `PluginDataEntryFunction2`
2. **Parameters**: Add/remove params in `ParamsSetup()` and the param enums in `.h`
3. **Compute shader**: Replace `shaders/gain.comp` with your effect logic
4. **Uniform buffer**: Update `GainUniforms` struct and shader `layout(std140)` to match
5. **Descriptor bindings**: If your shader needs more buffers/images, update `CreateDescriptorSetLayout()`, `UpdateDescriptorSet()`, and `CreateDescriptorPool()`
6. **Render method**: Extend `RenderGain()` to pass your custom data to the GPU

## Common Issues

| Problem | Solution |
|---------|----------|
| Plugin doesn't load in AE | Check that the `.aex` is in the correct Plug-ins folder. Check AE's console for errors. |
| Black output from GPU | Enable Vulkan validation layers (uncomment in `CreateVulkanInstance`) to see shader errors. |
| GPU path not activating | Check "GPU Enable" dropdown is set to GPU. Check AE console for Vulkan init errors. |
| Slow download (>400ms at 4K) | Verify HOST_CACHED memory is being used. Fallback to HOST_COHERENT is much slower. |
| Crash during Play mode | Thread safety issue - ensure per-thread command pools and queue mutex are working. |
| Shader compilation error | Run `glslc gain.comp` manually to see GLSL errors. Ensure Vulkan SDK is installed. |
