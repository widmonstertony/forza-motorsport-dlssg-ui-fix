// SPDX-License-Identifier: MIT
// Forza Motorsport HUDless MFG bridge.
// Captures the final scene immediately around the game's HUD pass, builds an
// premultiplied RGBA UI layer on the GPU, and supplies
// HUDLessColor + UIAlpha to Streamline.

#include <windows.h>
#include <psapi.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <include/reshade.hpp>
#include <sl.h>
#include <sl_dlss_g.h>
#include <nvsdk_ngx.h>

#include "ui_mask_dxil.hpp"
#include "../third_party/mfgunlock/ngx_hook.hpp"

namespace {
using Microsoft::WRL::ComPtr;

constexpr char kConfigSection[] = "ForzaMotorsport.HUDlessMFG";
constexpr uint64_t kHudPixelShader = 0x5412C343052D1BCAull;
constexpr uint32_t kSlotCount = 4;

std::atomic_bool g_enabled{true};
std::atomic_bool g_hooks_installed{false};
std::atomic_bool g_loader_hooked{false};
std::atomic<uint32_t> g_submitted_slot{UINT32_MAX};
std::atomic<uint32_t> g_tagged_frames{0};
std::atomic<uint32_t> g_captured_frames{0};
std::atomic<uint32_t> g_mask_frames{0};
std::atomic<uint32_t> g_failures{0};
std::atomic<uint32_t> g_fallback_slot_counter{0};
std::atomic<uint32_t> g_ready_slot{UINT32_MAX};
std::atomic<uint32_t> g_debug_begin_count{0};
std::atomic<uint32_t> g_debug_ui_draw_count{0};
std::atomic<uint32_t> g_debug_end_count{0};
std::atomic<uint32_t> g_debug_execute_count{0};
std::atomic<uint32_t> g_debug_present_count{0};
std::atomic<float> g_threshold{0.0010f};
std::atomic<float> g_gain{128.0f};
int g_probe_level = 2;
PFun_slGetNativeInterface* g_get_native_interface = nullptr;
using SetTagFn = sl::Result (*)(const sl::ViewportHandle&, const sl::ResourceTag*,
                                uint32_t, sl::CommandBuffer*);
SetTagFn g_set_tag = nullptr;
PFun_slSetTagForFrame* g_set_tag_for_frame = nullptr;
std::atomic_bool g_logged_tag_command_mapping{false};
std::atomic_bool g_logged_reshade_command_mapping{false};
std::atomic_bool g_logged_first_tag_call{false};

uint32_t g_output_width = 0;
uint32_t g_output_height = 0;
DXGI_FORMAT g_backbuffer_format = DXGI_FORMAT_UNKNOWN;

uint64_t Fnv1a(const void* bytes, size_t size) {
  const auto* data = static_cast<const uint8_t*>(bytes);
  uint64_t hash = 14695981039346656037ull;
  for (size_t index = 0; index < size; ++index) {
    hash ^= data[index];
    hash *= 1099511628211ull;
  }
  return hash;
}

void Log(reshade::log::level level, const std::string& message) {
  reshade::log::message(level, ("fm-hudless-mfg: " + message).c_str());
}

struct Slot {
  ComPtr<ID3D12Resource> hudless;
  ComPtr<ID3D12Resource> with_ui;
  ComPtr<ID3D12Resource> ui_alpha;
  ComPtr<ID3D12Resource> ui_color;
  ComPtr<ID3D12Resource> display_hudless;
  ComPtr<ID3D12Resource> final_hudless;
  D3D12_RESOURCE_STATES hudless_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  D3D12_RESOURCE_STATES with_ui_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  D3D12_RESOURCE_STATES alpha_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  D3D12_RESOURCE_STATES ui_color_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  D3D12_RESOURCE_STATES display_hudless_state =
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  D3D12_RESOURCE_STATES final_hudless_state =
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  sl::Resource hudless_sl{};
  sl::Resource display_hudless_sl{};
  sl::Resource alpha_sl{};
  sl::Resource ui_color_sl{};
  sl::Extent extent{};
};

struct MaskCommandContext {
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  uint64_t fence_value = 0;
};

struct Pool {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12DescriptorHeap> descriptors;
  ComPtr<ID3D12RootSignature> root_signature;
  ComPtr<ID3D12PipelineState> mask_pipeline;
  ComPtr<ID3D12Fence> mask_fence;
  std::array<Slot, kSlotCount> slots;
  std::array<MaskCommandContext, kSlotCount> mask_commands;
  D3D12_RESOURCE_DESC color_desc{};
  uint32_t descriptor_size = 0;
  uint64_t next_fence_value = 1;
  bool ready = false;
};

struct CommandState {
  uint64_t pipeline = 0;
  reshade::api::resource target{};
  uint32_t slot = UINT32_MAX;
  bool in_render_pass = false;
  bool ui_seen = false;
  bool pre_captured = false;
  bool post_captured = false;
};

std::mutex g_mutex;
Pool g_pool;
std::unordered_map<uint64_t, uint64_t> g_pipeline_hashes;
std::unordered_map<uint64_t, CommandState> g_commands;
std::unordered_set<uint64_t> g_known_ui_targets;
thread_local bool g_creating_internal_pipeline = false;
thread_local bool g_recording_internal_commands = false;

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource,
                                  D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after);

using AcquireMotionResourceFn = BOOL (WINAPI*)(
    ID3D12Resource**, uint32_t*, uint32_t*, uint32_t*, uint32_t*, uint32_t*);

struct MotionCaptureJob {
  ComPtr<ID3D12Resource> source;
  ComPtr<ID3D12Resource> readback;
  ComPtr<ID3D12Fence> fence;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT rows = 0;
  UINT64 row_bytes = 0;
  UINT64 total_bytes = 0;
  uint32_t source_state = UINT_MAX;
  uint32_t active_left = 0;
  uint32_t active_top = 0;
  uint32_t active_width = 0;
  uint32_t active_height = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  uint64_t fence_value = 0;
  bool recorded = false;
  bool signal_requested = false;
  bool written = false;
};

std::mutex g_motion_capture_mutex;
MotionCaptureJob g_motion_capture;
std::atomic_bool g_motion_capture_attempted{false};

std::wstring ModuleDirectory(HMODULE module) {
  wchar_t path[32768]{};
  GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
  std::wstring result(path);
  const size_t slash = result.find_last_of(L"\\/");
  return slash == std::wstring::npos ? L"" : result.substr(0, slash + 1);
}

void RecordMotionCapture(reshade::api::command_list* command) {
  if (command == nullptr || g_motion_capture_attempted.load()) return;
  HMODULE transfusion = GetModuleHandleW(L"version.dll");
  const std::wstring directory = ModuleDirectory(transfusion);
  const std::wstring request_path = directory + L"motorsport-motion.request";
  if (GetFileAttributesW(request_path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
  if (g_motion_capture_attempted.exchange(true)) return;
  DeleteFileW(request_path.c_str());
  auto acquire = transfusion ? reinterpret_cast<AcquireMotionResourceFn>(
      GetProcAddress(transfusion, "TransfusionAcquireMotionResource")) : nullptr;
  if (acquire == nullptr) {
    g_motion_capture_attempted.store(false);
    return;
  }

  ID3D12Resource* source_raw = nullptr;
  uint32_t state = UINT_MAX, left = 0, top = 0, width = 0, height = 0;
  if (!acquire(&source_raw, &state, &left, &top, &width, &height) ||
      source_raw == nullptr || state == UINT_MAX) {
    if (source_raw) source_raw->Release();
    g_motion_capture_attempted.store(false);
    return;
  }
  ComPtr<ID3D12Resource> source;
  source.Attach(source_raw); // Transfusion returned one retained reference.
  ComPtr<ID3D12Device> device;
  if (FAILED(source->GetDevice(IID_PPV_ARGS(&device)))) {
    g_motion_capture_attempted.store(false);
    return;
  }
  const D3D12_RESOURCE_DESC desc = source->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT rows = 0;
  UINT64 row_bytes = 0, total_bytes = 0;
  device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows,
                                &row_bytes, &total_bytes);
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
      desc.Format != DXGI_FORMAT_R16G16_FLOAT || total_bytes == 0) {
    Log(reshade::log::level::warning,
        "MV capture skipped: unexpected motion-resource description");
    return;
  }

  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC buffer{};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = total_bytes;
  buffer.Height = 1;
  buffer.DepthOrArraySize = 1;
  buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> readback;
  ComPtr<ID3D12Fence> fence;
  if (FAILED(device->CreateCommittedResource(
          &heap, D3D12_HEAP_FLAG_NONE, &buffer,
          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
          IID_PPV_ARGS(&readback))) ||
      FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                 IID_PPV_ARGS(&fence)))) {
    Log(reshade::log::level::warning,
        "MV capture skipped: readback allocation failed");
    return;
  }

  auto* list = reinterpret_cast<ID3D12GraphicsCommandList*>(command->get_native());
  if (list == nullptr) {
    g_motion_capture_attempted.store(false);
    return;
  }
  const auto source_state = static_cast<D3D12_RESOURCE_STATES>(state);
  if (source_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
    const auto barrier = Transition(source.Get(), source_state,
                                    D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &barrier);
  }
  D3D12_TEXTURE_COPY_LOCATION src{};
  src.pResource = source.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION dst{};
  dst.pResource = readback.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = footprint;
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  if (source_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
    const auto barrier = Transition(source.Get(),
                                    D3D12_RESOURCE_STATE_COPY_SOURCE,
                                    source_state);
    list->ResourceBarrier(1, &barrier);
  }

  std::lock_guard lock(g_motion_capture_mutex);
  g_motion_capture.source = std::move(source);
  g_motion_capture.readback = std::move(readback);
  g_motion_capture.fence = std::move(fence);
  g_motion_capture.footprint = footprint;
  g_motion_capture.rows = rows;
  g_motion_capture.row_bytes = row_bytes;
  g_motion_capture.total_bytes = total_bytes;
  g_motion_capture.source_state = state;
  g_motion_capture.active_left = left;
  g_motion_capture.active_top = top;
  g_motion_capture.active_width = width;
  g_motion_capture.active_height = height;
  g_motion_capture.format = desc.Format;
  g_motion_capture.recorded = true;
  g_motion_capture.signal_requested = false;
  Log(reshade::log::level::info,
      "recorded one read-only copy of the game's RG16F motion-vector texture");
}

void CompleteMotionCapture(reshade::api::command_queue* queue) {
  if (queue == nullptr) return;
  auto* native_queue = reinterpret_cast<ID3D12CommandQueue*>(queue->get_native());
  if (native_queue == nullptr) return;
  std::lock_guard lock(g_motion_capture_mutex);
  MotionCaptureJob& job = g_motion_capture;
  if (!job.recorded || job.written || job.fence == nullptr) return;
  if (!job.signal_requested) {
    if (SUCCEEDED(native_queue->Signal(job.fence.Get(), 1))) {
      job.fence_value = 1;
      job.signal_requested = true;
    }
    return;
  }
  if (job.fence->GetCompletedValue() < job.fence_value) return;

  void* mapped = nullptr;
  D3D12_RANGE read_range{0, static_cast<SIZE_T>(job.total_bytes)};
  bool ok = SUCCEEDED(job.readback->Map(0, &read_range, &mapped)) && mapped;
  HMODULE transfusion = GetModuleHandleW(L"version.dll");
  const std::wstring directory = ModuleDirectory(transfusion);
  const std::wstring raw_path = directory + L"motorsport-motion-rg16f.raw";
  const std::wstring meta_path = directory + L"motorsport-motion-rg16f.json";
  if (ok) {
    FILE* raw = nullptr;
    ok = _wfopen_s(&raw, raw_path.c_str(), L"wb") == 0 && raw != nullptr;
    if (ok) {
      ok = fwrite(mapped, 1, static_cast<size_t>(job.total_bytes), raw) ==
          static_cast<size_t>(job.total_bytes);
      fclose(raw);
    }
    D3D12_RANGE empty{0, 0};
    job.readback->Unmap(0, &empty);
  }
  if (ok) {
    FILE* meta = nullptr;
    if (_wfopen_s(&meta, meta_path.c_str(), L"wb") == 0 && meta) {
      fprintf(meta,
          "{\n  \"textureWidth\": %u,\n  \"textureHeight\": %u,\n"
          "  \"format\": %u,\n  \"rowPitch\": %u,\n"
          "  \"activeRect\": [%u, %u, %u, %u],\n  \"bytes\": %llu\n}\n",
          job.footprint.Footprint.Width, job.footprint.Footprint.Height,
          static_cast<uint32_t>(job.format), job.footprint.Footprint.RowPitch,
          job.active_left, job.active_top, job.active_width, job.active_height,
          static_cast<unsigned long long>(job.total_bytes));
      fclose(meta);
    }
    Log(reshade::log::level::info,
        "saved the game's raw motion-vector texture for analysis");
  } else {
    Log(reshade::log::level::error,
        "failed to write the game's raw motion-vector texture");
  }
  job.written = true;
  job.source.Reset();
  job.readback.Reset();
  job.fence.Reset();
}

struct InternalPipelineGuard {
  InternalPipelineGuard() { g_creating_internal_pipeline = true; }
  ~InternalPipelineGuard() { g_creating_internal_pipeline = false; }
};

struct InternalCommandGuard {
  InternalCommandGuard() { g_recording_internal_commands = true; }
  ~InternalCommandGuard() { g_recording_internal_commands = false; }
};

void TryInstallHooks();
void TryInstallNgxHook();

uint64_t NormalizeCommandPointer(void* pointer, bool from_tag) {
  if (pointer == nullptr) return 0;
  void* native = nullptr;
  if (g_get_native_interface != nullptr &&
      g_get_native_interface(pointer, &native) == sl::Result::eOk &&
      native != nullptr) {
    auto& logged = from_tag ? g_logged_tag_command_mapping
                            : g_logged_reshade_command_mapping;
    if (!logged.exchange(true)) {
      std::ostringstream message;
      message << (from_tag ? "normalized Streamline tag command "
                           : "normalized ReShade command ")
              << pointer << " to native " << native;
      Log(reshade::log::level::info, message.str());
    }
    return reinterpret_cast<uint64_t>(native);
  }
  return reinterpret_cast<uint64_t>(pointer);
}

uint64_t CommandKey(reshade::api::command_list* command) {
  // ReShade already exposes the native D3D12 command-list pointer here.
  // Passing that pointer back through Streamline's wrapper-unwrapping helper
  // is unnecessary and trips Transfusion's feature-creation path.
  return command->get_native();
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource,
                                  D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.StateBefore = before;
  barrier.Transition.StateAfter = after;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  return barrier;
}

bool CreateColorCopy(ID3D12Device* device, const D3D12_RESOURCE_DESC& source,
                     ID3D12Resource** result) {
  D3D12_RESOURCE_DESC desc = source;
  desc.Flags = D3D12_RESOURCE_FLAG_NONE;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  return SUCCEEDED(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &desc,
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
      IID_PPV_ARGS(result)));
}

bool CreateAlpha(ID3D12Device* device, uint32_t width, uint32_t height,
                 ID3D12Resource** result) {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  // Streamline explicitly prefers a one-channel UI alpha mask when available.
  // Keeping UI colour out of this surface also avoids HDR10/SDR colour-space
  // ambiguity in the provider's UIColorAndAlpha path.
  desc.Format = DXGI_FORMAT_R8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  return SUCCEEDED(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &desc,
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
      IID_PPV_ARGS(result)));
}

bool CreateDisplayHudless(ID3D12Device* device, uint32_t width, uint32_t height,
                          ID3D12Resource** result) {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  return SUCCEEDED(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &desc,
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
      IID_PPV_ARGS(result)));
}

bool CreateUiColor(ID3D12Device* device, uint32_t width, uint32_t height,
                   ID3D12Resource** result) {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  // HDR10 values remain display-encoded UNORM values. Four 16-bit channels
  // preserve enough alpha precision and avoid unsupported FP16/scRGB input.
  desc.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  return SUCCEEDED(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &desc,
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
      IID_PPV_ARGS(result)));
}

bool CreateComputeObjects(Pool& pool) {
  D3D12_DESCRIPTOR_RANGE ranges[2]{};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[0].NumDescriptors = 3;
  ranges[0].BaseShaderRegister = 0;
  ranges[0].RegisterSpace = 0;
  ranges[0].OffsetInDescriptorsFromTableStart = 0;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[1].NumDescriptors = 3;
  ranges[1].BaseShaderRegister = 0;
  ranges[1].RegisterSpace = 0;
  ranges[1].OffsetInDescriptorsFromTableStart = 3;

  D3D12_ROOT_PARAMETER parameters[2]{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[0].DescriptorTable.NumDescriptorRanges = 2;
  parameters[0].DescriptorTable.pDescriptorRanges = ranges;
  parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[1].Constants.ShaderRegister = 0;
  parameters[1].Constants.RegisterSpace = 0;
  parameters[1].Constants.Num32BitValues = 4;
  parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  D3D12_ROOT_SIGNATURE_DESC root_desc{};
  root_desc.NumParameters = 2;
  root_desc.pParameters = parameters;
  root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  ComPtr<ID3DBlob> signature;
  ComPtr<ID3DBlob> error;
  if (FAILED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                         &signature, &error))) {
    if (error) Log(reshade::log::level::error,
                   std::string(static_cast<const char*>(error->GetBufferPointer()),
                               error->GetBufferSize()));
    return false;
  }
  if (FAILED(pool.device->CreateRootSignature(
          0, signature->GetBufferPointer(), signature->GetBufferSize(),
          IID_PPV_ARGS(&pool.root_signature)))) return false;

  D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
  pipeline.pRootSignature = pool.root_signature.Get();
  pipeline.CS.pShaderBytecode = kUiMaskDxil;
  pipeline.CS.BytecodeLength = kUiMaskDxilSize;
  return SUCCEEDED(pool.device->CreateComputePipelineState(
      &pipeline, IID_PPV_ARGS(&pool.mask_pipeline)));
}

bool EnsurePool(ID3D12Resource* target, ID3D12GraphicsCommandList* list) {
  if (target == nullptr || list == nullptr || g_output_width == 0 ||
      g_output_height == 0) return false;
  std::lock_guard lock(g_mutex);
  const D3D12_RESOURCE_DESC desc = target->GetDesc();
  if (g_pool.ready) {
    return g_pool.color_desc.Width == desc.Width &&
           g_pool.color_desc.Height == desc.Height &&
           g_pool.color_desc.Format == desc.Format;
  }
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
      desc.Width != g_output_width || desc.Height != g_output_height ||
      desc.SampleDesc.Count != 1) return false;
  // Create every object that will be bound to a command list through that
  // command list's own device.  Motorsport has both Streamline and ReShade
  // D3D12 proxies in the process; asking the render target for its device can
  // return a different proxy layer.  Passing a descriptor heap from that layer
  // to SetDescriptorHeaps on ReShade's list crashes before the call returns.
  if (FAILED(list->GetDevice(IID_PPV_ARGS(&g_pool.device)))) return false;
  {
    std::ostringstream message;
    message << "creating HUD separation objects from command-list device="
            << g_pool.device.Get() << " list=" << list << " target=" << target;
    Log(reshade::log::level::info, message.str());
  }
  g_pool.color_desc = desc;
  // Creating our mask PSO re-enters ReShade's init_pipeline callback on this
  // same thread.  EnsurePool owns g_mutex here, so that callback must ignore
  // the add-on's own PSO instead of attempting to lock g_mutex recursively.
  InternalPipelineGuard internal_pipeline_guard;
  if (!CreateComputeObjects(g_pool)) {
    ++g_failures;
    Log(reshade::log::level::error, "could not create the UI-mask compute pipeline");
    return false;
  }

  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heap_desc.NumDescriptors = kSlotCount * 6;
  heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(g_pool.device->CreateDescriptorHeap(
          &heap_desc, IID_PPV_ARGS(&g_pool.descriptors)))) return false;
  g_pool.descriptor_size = g_pool.device->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

  auto cpu = g_pool.descriptors->GetCPUDescriptorHandleForHeapStart();
  if (FAILED(g_pool.device->CreateFence(
          0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_pool.mask_fence))))
    return false;
  // D3D12 command-list creation/initial close is observable by ReShade and
  // synchronously re-enters reset/close callbacks.  EnsurePool already owns
  // g_mutex, so those callbacks must recognize this list as internal.
  InternalCommandGuard internal_commands;
  for (uint32_t index = 0; index < kSlotCount; ++index) {
    Slot& slot = g_pool.slots[index];
    MaskCommandContext& context = g_pool.mask_commands[index];
    if (FAILED(g_pool.device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&context.allocator))) ||
        FAILED(g_pool.device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, context.allocator.Get(),
            g_pool.mask_pipeline.Get(), IID_PPV_ARGS(&context.list))) ||
        FAILED(context.list->Close())) {
      ++g_failures;
      Log(reshade::log::level::error,
          "could not create an owned UI-mask command list");
      return false;
    }
    if (!CreateColorCopy(g_pool.device.Get(), desc, &slot.hudless) ||
        !CreateColorCopy(g_pool.device.Get(), desc, &slot.with_ui) ||
        !CreateAlpha(g_pool.device.Get(), static_cast<uint32_t>(desc.Width),
                     desc.Height, &slot.ui_alpha) ||
        !CreateUiColor(g_pool.device.Get(), static_cast<uint32_t>(desc.Width),
                       desc.Height, &slot.ui_color) ||
        !CreateDisplayHudless(g_pool.device.Get(),
                             static_cast<uint32_t>(desc.Width), desc.Height,
                             &slot.display_hudless)) {
      ++g_failures;
      Log(reshade::log::level::error, "could not allocate the HUD separation buffers");
      return false;
    }
    D3D12_RESOURCE_DESC final_desc = desc;
    final_desc.Format = g_backbuffer_format;
    final_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (!CreateColorCopy(g_pool.device.Get(), final_desc,
                         &slot.final_hudless)) {
      ++g_failures;
      Log(reshade::log::level::error,
          "could not allocate the final HDR10 snapshot buffer");
      return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = desc.Format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE handle = cpu;
    handle.ptr += static_cast<SIZE_T>(index * 6) * g_pool.descriptor_size;
    g_pool.device->CreateShaderResourceView(slot.hudless.Get(), &srv, handle);
    handle.ptr += g_pool.descriptor_size;
    g_pool.device->CreateShaderResourceView(slot.with_ui.Get(), &srv, handle);
    handle.ptr += g_pool.descriptor_size;
    srv.Format = g_backbuffer_format;
    g_pool.device->CreateShaderResourceView(slot.final_hudless.Get(), &srv,
                                            handle);
    handle.ptr += g_pool.descriptor_size;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R8_UNORM;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_pool.device->CreateUnorderedAccessView(slot.ui_alpha.Get(), nullptr, &uav, handle);
    handle.ptr += g_pool.descriptor_size;
    uav.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    g_pool.device->CreateUnorderedAccessView(
        slot.display_hudless.Get(), nullptr, &uav, handle);
    handle.ptr += g_pool.descriptor_size;
    uav.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
    g_pool.device->CreateUnorderedAccessView(
        slot.ui_color.Get(), nullptr, &uav, handle);

    slot.hudless_sl = sl::Resource(sl::ResourceType::eTex2d, slot.hudless.Get(),
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    slot.hudless_sl.width = static_cast<uint32_t>(desc.Width);
    slot.hudless_sl.height = desc.Height;
    slot.hudless_sl.nativeFormat = static_cast<uint32_t>(desc.Format);
    slot.hudless_sl.mipLevels = 1;
    slot.hudless_sl.arrayLayers = 1;
    slot.display_hudless_sl = sl::Resource(
        sl::ResourceType::eTex2d, slot.display_hudless.Get(),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    slot.display_hudless_sl.width = static_cast<uint32_t>(desc.Width);
    slot.display_hudless_sl.height = desc.Height;
    slot.display_hudless_sl.nativeFormat =
        static_cast<uint32_t>(DXGI_FORMAT_R10G10B10A2_UNORM);
    slot.display_hudless_sl.mipLevels = 1;
    slot.display_hudless_sl.arrayLayers = 1;
    slot.alpha_sl = sl::Resource(sl::ResourceType::eTex2d, slot.ui_alpha.Get(),
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    slot.alpha_sl.width = static_cast<uint32_t>(desc.Width);
    slot.alpha_sl.height = desc.Height;
    slot.alpha_sl.nativeFormat =
        static_cast<uint32_t>(DXGI_FORMAT_R8_UNORM);
    slot.alpha_sl.mipLevels = 1;
    slot.alpha_sl.arrayLayers = 1;
    slot.ui_color_sl = sl::Resource(
        sl::ResourceType::eTex2d, slot.ui_color.Get(),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    slot.ui_color_sl.width = static_cast<uint32_t>(desc.Width);
    slot.ui_color_sl.height = desc.Height;
    slot.ui_color_sl.nativeFormat =
        static_cast<uint32_t>(DXGI_FORMAT_R16G16B16A16_UNORM);
    slot.ui_color_sl.mipLevels = 1;
    slot.ui_color_sl.arrayLayers = 1;
    slot.extent.left = 0;
    slot.extent.top = 0;
    slot.extent.width = static_cast<uint32_t>(desc.Width);
    slot.extent.height = desc.Height;
  }
  g_pool.ready = true;
  std::ostringstream message;
  message << "created " << desc.Width << "x" << desc.Height
          << " HUDless/UI buffers, DXGI format " << static_cast<uint32_t>(desc.Format)
          << "; backbuffer format " << static_cast<uint32_t>(g_backbuffer_format);
  Log(reshade::log::level::info, message.str());
  return true;
}

bool CopyColor(ID3D12GraphicsCommandList* list, ID3D12Resource* source,
               ID3D12Resource* destination, D3D12_RESOURCE_STATES& destination_state,
               D3D12_RESOURCE_STATES source_state = D3D12_RESOURCE_STATE_RENDER_TARGET) {
  if (list == nullptr || source == nullptr || destination == nullptr) return false;
  std::array<D3D12_RESOURCE_BARRIER, 2> before{};
  uint32_t count = 0;
  before[count++] = Transition(source, source_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
  if (destination_state != D3D12_RESOURCE_STATE_COPY_DEST) {
    before[count++] = Transition(destination, destination_state,
                                 D3D12_RESOURCE_STATE_COPY_DEST);
  }
  list->ResourceBarrier(count, before.data());
  list->CopyResource(destination, source);
  const std::array<D3D12_RESOURCE_BARRIER, 2> after = {
      Transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE, source_state),
      Transition(destination, D3D12_RESOURCE_STATE_COPY_DEST,
                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
  list->ResourceBarrier(static_cast<UINT>(after.size()), after.data());
  destination_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  return true;
}

bool CapturePre(reshade::api::command_list* command, ID3D12Resource* target,
                uint32_t slot_index) {
  auto* list =
      reinterpret_cast<ID3D12GraphicsCommandList*>(command->get_native());
  if (!EnsurePool(target, list) || slot_index >= kSlotCount) return false;
  std::lock_guard lock(g_mutex);
  Slot& slot = g_pool.slots[slot_index];
  return CopyColor(list, target, slot.hudless.Get(), slot.hudless_state);
}

bool CapturePost(reshade::api::command_list* command, ID3D12Resource* target,
                 uint32_t slot_index) {
  if (!g_pool.ready || slot_index >= kSlotCount) return false;
  std::lock_guard lock(g_mutex);
  Slot& slot = g_pool.slots[slot_index];
  const bool result = CopyColor(
      reinterpret_cast<ID3D12GraphicsCommandList*>(command->get_native()), target,
      slot.with_ui.Get(), slot.with_ui_state);
  if (result && g_captured_frames.fetch_add(1) == 0)
    Log(reshade::log::level::info, "captured the first complete pre-UI/post-UI pair");
  return result;
}

void OnInitSwapchain(reshade::api::swapchain* swapchain, bool) {
  if (swapchain->get_device()->get_api() != reshade::api::device_api::d3d12 ||
      swapchain->get_back_buffer_count() == 0) return;
  const auto desc = swapchain->get_device()->get_resource_desc(swapchain->get_back_buffer(0));
  const DXGI_FORMAT new_format = static_cast<DXGI_FORMAT>(desc.texture.format);
  {
    std::lock_guard lock(g_mutex);
    if (g_pool.ready &&
        (g_pool.color_desc.Width != desc.texture.width ||
         g_pool.color_desc.Height != desc.texture.height ||
         g_backbuffer_format != new_format)) {
      std::ostringstream message;
      message << "swap-chain changed from " << g_pool.color_desc.Width << "x"
              << g_pool.color_desc.Height << " format "
              << static_cast<uint32_t>(g_backbuffer_format) << " to "
              << desc.texture.width << "x" << desc.texture.height << " format "
              << static_cast<uint32_t>(new_format)
              << "; rebuilding HUD separation resources";
      Log(reshade::log::level::info, message.str());
      g_pool = Pool{};
      g_commands.clear();
      g_known_ui_targets.clear();
      g_submitted_slot.store(UINT32_MAX, std::memory_order_release);
      g_ready_slot.store(UINT32_MAX, std::memory_order_release);
    }
  }
  g_output_width = desc.texture.width;
  g_output_height = desc.texture.height;
  g_backbuffer_format = new_format;

  // Keep the bridge on Streamline's public resource-tag path.  Transfusion
  // enables UI recomposition only after it has observed valid
  // HUDless/UIAlpha
  // resources, so startup never enters the unsafe null-resource UIR path.
  TryInstallHooks();
}

void OnInitPipeline(reshade::api::device*, reshade::api::pipeline_layout, uint32_t count,
                    const reshade::api::pipeline_subobject* subobjects,
                    reshade::api::pipeline pipeline) {
  if (g_creating_internal_pipeline) return;
  uint64_t hash = 0;
  for (uint32_t index = 0; index < count; ++index) {
    if (subobjects[index].type != reshade::api::pipeline_subobject_type::pixel_shader ||
        subobjects[index].count == 0 || subobjects[index].data == nullptr) continue;
    const auto& shader = *static_cast<const reshade::api::shader_desc*>(subobjects[index].data);
    if (shader.code != nullptr && shader.code_size != 0)
      hash = Fnv1a(shader.code, shader.code_size);
  }
  std::lock_guard lock(g_mutex);
  g_pipeline_hashes[pipeline.handle] = hash;
}

void OnDestroyPipeline(reshade::api::device*, reshade::api::pipeline pipeline) {
  if (g_creating_internal_pipeline) return;
  std::lock_guard lock(g_mutex);
  g_pipeline_hashes.erase(pipeline.handle);
}

void OnResetCommandList(reshade::api::command_list* command) {
  if (g_recording_internal_commands) return;
  std::lock_guard lock(g_mutex);
  g_commands[CommandKey(command)] = {};
}

reshade::api::resource ResolveTarget(reshade::api::command_list* command,
                                    uint32_t count,
                                    const reshade::api::resource_view* views) {
  reshade::api::resource target{};
  for (uint32_t index = 0; index < count; ++index) {
    if (views[index].handle == 0) continue;
    const auto resource = command->get_device()->get_resource_from_view(views[index]);
    if (resource.handle == 0) continue;
    const auto desc = command->get_device()->get_resource_desc(resource);
    if (desc.texture.width == g_output_width && desc.texture.height == g_output_height) {
      target = resource;
      break;
    }
  }
  return target;
}

void SetTarget(reshade::api::command_list* command,
               reshade::api::resource target) {
  std::lock_guard lock(g_mutex);
  g_commands[CommandKey(command)].target = target;
}

void SetTarget(reshade::api::command_list* command, uint32_t count,
               const reshade::api::resource_view* views) {
  SetTarget(command, ResolveTarget(command, count, views));
}

bool FinalizeClassicUiSequence(reshade::api::command_list* command,
                               const char* reason) {
  reshade::api::resource target{};
  uint32_t slot = UINT32_MAX;
  bool should_capture = false;
  {
    std::lock_guard lock(g_mutex);
    const auto found = g_commands.find(CommandKey(command));
    if (found == g_commands.end()) return false;
    const CommandState& state = found->second;
    target = state.target;
    slot = state.slot;
    should_capture = !state.in_render_pass && state.ui_seen &&
                     state.pre_captured && !state.post_captured &&
                     target.handle != 0 && slot < kSlotCount;
  }
  if (!should_capture) return false;

  const bool captured = CapturePost(
      command, reinterpret_cast<ID3D12Resource*>(target.handle), slot);
  if (captured) {
    std::lock_guard lock(g_mutex);
    CommandState& state = g_commands[CommandKey(command)];
    if (state.target.handle == target.handle && state.slot == slot)
      state.post_captured = true;
  }

  static std::atomic_uint32_t debug_finalize_count = 0;
  const uint32_t debug_index = debug_finalize_count.fetch_add(1);
  if (debug_index < 24) {
    std::ostringstream message;
    message << "classic UI finalize #" << debug_index
            << " reason=" << reason
            << " command=0x" << std::hex << CommandKey(command)
            << " target=0x" << target.handle << std::dec
            << " slot=" << slot << " captured=" << captured;
    Log(reshade::log::level::info, message.str());
  }
  return captured;
}

void OnBindTargets(reshade::api::command_list* command, uint32_t count,
                   const reshade::api::resource_view* views,
                   reshade::api::resource_view) {
  if (g_recording_internal_commands) return;
  const reshade::api::resource next_target = ResolveTarget(command, count, views);
  bool target_changed = false;
  {
    std::lock_guard lock(g_mutex);
    const auto found = g_commands.find(CommandKey(command));
    target_changed = found != g_commands.end() &&
                     found->second.target.handle != 0 &&
                     found->second.target.handle != next_target.handle;
  }
  if (target_changed) FinalizeClassicUiSequence(command, "target-change");
  SetTarget(command, next_target);
}

bool OnBeginRenderPass(reshade::api::command_list* command, uint32_t count,
                       const reshade::api::render_pass_render_target_desc* targets,
                       const reshade::api::render_pass_depth_stencil_desc*,
                       reshade::api::render_pass_flags) {
  if (g_recording_internal_commands) return false;
  std::vector<reshade::api::resource_view> views;
  views.reserve(count);
  for (uint32_t index = 0; index < count; ++index) views.push_back(targets[index].view);
  SetTarget(command, count, views.data());

  reshade::api::resource target{};
  uint32_t slot = UINT32_MAX;
  bool known = false;
  {
    std::lock_guard lock(g_mutex);
    CommandState& state = g_commands[CommandKey(command)];
    state.in_render_pass = true;
    state.ui_seen = false;
    state.pre_captured = false;
    target = state.target;
    slot = state.slot;
    known = target.handle != 0 && g_known_ui_targets.contains(target.handle);
    if (known && slot >= kSlotCount) {
      slot = g_fallback_slot_counter.fetch_add(1, std::memory_order_relaxed) %
             kSlotCount;
      state.slot = slot;
    }
  }
  if (known && slot < kSlotCount) {
    const bool captured = CapturePre(command,
        reinterpret_cast<ID3D12Resource*>(target.handle), slot);
    std::lock_guard lock(g_mutex);
    g_commands[CommandKey(command)].pre_captured = captured;
  }
  const uint32_t debug_index = g_debug_begin_count.fetch_add(1);
  if (debug_index < 12 || (known && debug_index < 64)) {
    std::ostringstream message;
    message << "begin_render_pass #" << debug_index
            << " target=0x" << std::hex << target.handle << std::dec
            << " known=" << known << " slot=" << slot
            << " pre=" << (known && slot < kSlotCount);
    Log(reshade::log::level::info, message.str());
  }
  return false;
}

void OnBindPipeline(reshade::api::command_list* command, reshade::api::pipeline_stage,
                    reshade::api::pipeline pipeline) {
  if (g_recording_internal_commands) return;
  bool leaving_hud_batch = false;
  {
    std::lock_guard lock(g_mutex);
    CommandState& state = g_commands[CommandKey(command)];
    const auto previous = g_pipeline_hashes.find(state.pipeline);
    const auto next = g_pipeline_hashes.find(pipeline.handle);
    const bool previous_is_hud =
        previous != g_pipeline_hashes.end() && previous->second == kHudPixelShader;
    const bool next_is_hud =
        next != g_pipeline_hashes.end() && next->second == kHudPixelShader;
    leaving_hud_batch = previous_is_hud && !next_is_hud && state.ui_seen &&
                        state.pre_captured && !state.post_captured;
  }
  // The classic D3D12 path does not use Begin/EndRenderPass. Capturing at
  // command-list close is too late: Motorsport runs additional full-screen
  // post-processing after the HUD, which turns nearly the whole difference
  // image into UI alpha. Capture at the first pipeline transition after the
  // contiguous HUD draw batch instead.
  if (leaving_hud_batch)
    FinalizeClassicUiSequence(command, "hud-pipeline-change");
  {
    std::lock_guard lock(g_mutex);
    g_commands[CommandKey(command)].pipeline = pipeline.handle;
  }
}

bool HandleHudDraw(reshade::api::command_list* command, const char* draw_kind,
                   uint32_t element_count, uint32_t instance_count,
                   uint32_t first_element, uint32_t first_instance) {
  if (g_recording_internal_commands) return false;
  reshade::api::resource target{};
  uint32_t slot = UINT32_MAX;
  bool is_ui = false;
  bool in_pass = false;
  bool needs_pre = false;
  {
    std::lock_guard lock(g_mutex);
    CommandState& state = g_commands[CommandKey(command)];
    const auto found = g_pipeline_hashes.find(state.pipeline);
    is_ui = found != g_pipeline_hashes.end() && found->second == kHudPixelShader;
    if (!is_ui) return false;
    target = state.target;
    slot = state.slot;
    in_pass = state.in_render_pass;
    needs_pre = !state.pre_captured;
    state.ui_seen = true;
    if (target.handle != 0) g_known_ui_targets.insert(target.handle);
    if (slot >= kSlotCount) {
      slot = g_fallback_slot_counter.fetch_add(1, std::memory_order_relaxed) %
             kSlotCount;
      state.slot = slot;
    }
  }
  if (target.handle == 0 || slot >= kSlotCount) return false;
  EnsurePool(
      reinterpret_cast<ID3D12Resource*>(target.handle),
      reinterpret_cast<ID3D12GraphicsCommandList*>(command->get_native()));
  const uint32_t debug_index = g_debug_ui_draw_count.fetch_add(1);
  if (debug_index < 24) {
    std::ostringstream message;
    message << "HUD draw #" << debug_index
            << " kind=" << draw_kind
            << " elements=" << element_count
            << " instances=" << instance_count
            << " first=" << first_element
            << " first_instance=" << first_instance
            << " command=0x" << std::hex << CommandKey(command)
            << " target=0x" << target.handle << std::dec
            << " slot=" << slot << " in_pass=" << in_pass
            << " needs_pre=" << needs_pre;
    Log(reshade::log::level::info, message.str());
  }
  if (!in_pass && needs_pre) {
    const bool captured = CapturePre(command,
        reinterpret_cast<ID3D12Resource*>(target.handle), slot);
    std::lock_guard lock(g_mutex);
    g_commands[CommandKey(command)].pre_captured = captured;
  }
  return false;
}

bool OnDraw(reshade::api::command_list* command, uint32_t vertex_count,
            uint32_t instance_count, uint32_t first_vertex,
            uint32_t first_instance) {
  return HandleHudDraw(command, "draw", vertex_count, instance_count,
                       first_vertex, first_instance);
}

bool OnDrawIndexed(reshade::api::command_list* command, uint32_t index_count,
                   uint32_t instance_count, uint32_t first_index,
                   int32_t, uint32_t first_instance) {
  return HandleHudDraw(command, "indexed", index_count, instance_count,
                       first_index, first_instance);
}

bool OnEndRenderPass(reshade::api::command_list* command) {
  if (g_recording_internal_commands) return false;
  reshade::api::resource target{};
  uint32_t slot = UINT32_MAX;
  bool should_capture = false;
  {
    std::lock_guard lock(g_mutex);
    CommandState& state = g_commands[CommandKey(command)];
    state.in_render_pass = false;
    target = state.target;
    slot = state.slot;
    should_capture = state.ui_seen && state.pre_captured &&
                     target.handle != 0 && slot < kSlotCount;
  }
  const bool captured = should_capture && CapturePost(
      command, reinterpret_cast<ID3D12Resource*>(target.handle), slot);
  const uint32_t debug_index = g_debug_end_count.fetch_add(1);
  if (debug_index < 24 || (should_capture && debug_index < 96)) {
    std::ostringstream message;
    message << "end_render_pass #" << debug_index
            << " command=0x" << std::hex << CommandKey(command)
            << " target=0x" << target.handle << std::dec
            << " slot=" << slot << " should_capture=" << should_capture
            << " captured=" << captured;
    Log(reshade::log::level::info, message.str());
  }
  if (captured) {
    std::lock_guard lock(g_mutex);
    CommandState& state = g_commands[CommandKey(command)];
    if (state.target.handle == target.handle && state.slot == slot)
      state.post_captured = true;
  }
  return false;
}

bool GenerateMaskAndPublish(reshade::api::command_list* command,
                            uint32_t index, const char* reason,
                            ID3D12Resource* final_color = nullptr) {
  const bool trace = g_mask_frames.load(std::memory_order_relaxed) == 0;
  if (trace)
    Log(reshade::log::level::info, "UI-mask trace: enter");
  if (command == nullptr || index >= kSlotCount) return false;
  auto* list = reinterpret_cast<ID3D12GraphicsCommandList*>(command->get_native());
  if (trace)
    Log(reshade::log::level::info, "UI-mask trace: native command list resolved");
  if (list == nullptr) return false;

  sl::Resource hudless_tag{};
  sl::Resource alpha_tag{};
  sl::Resource ui_color_tag{};
  sl::Extent tag_extent{};
  {
    std::lock_guard lock(g_mutex);
    if (trace)
      Log(reshade::log::level::info, "UI-mask trace: mutex acquired");
    if (!g_pool.ready) return false;
    Slot& slot = g_pool.slots[index];
    InternalCommandGuard internal_commands;
    if (final_color == nullptr || slot.final_hudless == nullptr) return false;
    const D3D12_RESOURCE_DESC final_desc = final_color->GetDesc();
    const D3D12_RESOURCE_DESC snapshot_desc = slot.final_hudless->GetDesc();
    if (final_desc.Width != snapshot_desc.Width ||
        final_desc.Height != snapshot_desc.Height ||
        final_desc.Format != snapshot_desc.Format) {
      if (g_failures.fetch_add(1) < 8)
        Log(reshade::log::level::error,
            "final HDR10 source does not match its snapshot resource");
      return false;
    }
    if (!CopyColor(list, final_color, slot.final_hudless.Get(),
                   slot.final_hudless_state,
                   D3D12_RESOURCE_STATE_RENDER_TARGET)) {
      ++g_failures;
      return false;
    }
    if (trace)
      Log(reshade::log::level::info, "UI-mask trace: recording resource transition");
    if (slot.alpha_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
      const auto barrier = Transition(slot.ui_alpha.Get(), slot.alpha_state,
                                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      list->ResourceBarrier(1, &barrier);
      slot.alpha_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    if (slot.display_hudless_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
      const auto barrier = Transition(
          slot.display_hudless.Get(), slot.display_hudless_state,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      list->ResourceBarrier(1, &barrier);
      slot.display_hudless_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    if (slot.ui_color_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
      const auto barrier = Transition(
          slot.ui_color.Get(), slot.ui_color_state,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      list->ResourceBarrier(1, &barrier);
      slot.ui_color_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    if (trace)
      Log(reshade::log::level::info, "UI-mask trace: binding compute state");
    ID3D12DescriptorHeap* heaps[] = {g_pool.descriptors.Get()};
    list->SetDescriptorHeaps(1, heaps);
    if (trace)
      Log(reshade::log::level::info, "UI-mask trace: descriptor heap bound");
    list->SetComputeRootSignature(g_pool.root_signature.Get());
    if (trace)
      Log(reshade::log::level::info, "UI-mask trace: root signature bound");
    list->SetPipelineState(g_pool.mask_pipeline.Get());
    if (trace)
      Log(reshade::log::level::info, "UI-mask trace: pipeline state bound");
    D3D12_GPU_DESCRIPTOR_HANDLE table =
        g_pool.descriptors->GetGPUDescriptorHandleForHeapStart();
    table.ptr += static_cast<UINT64>(index * 6) * g_pool.descriptor_size;
    list->SetComputeRootDescriptorTable(0, table);
    if (trace)
      Log(reshade::log::level::info, "UI-mask trace: descriptor table bound");
    struct Constants { uint32_t width, height; float threshold, gain; } settings{
        static_cast<uint32_t>(g_pool.color_desc.Width), g_pool.color_desc.Height,
        g_threshold.load(std::memory_order_relaxed),
        g_gain.load(std::memory_order_relaxed)};
    list->SetComputeRoot32BitConstants(1, 4, &settings, 0);
    if (trace)
      Log(reshade::log::level::info, "UI-mask trace: root constants bound");
    list->Dispatch((settings.width + 7) / 8, (settings.height + 7) / 8, 1);
    if (trace)
      Log(reshade::log::level::info, "UI-mask trace: dispatch recorded");
    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = slot.ui_alpha.Get();
    list->ResourceBarrier(1, &uav);
    uav.UAV.pResource = slot.display_hudless.Get();
    list->ResourceBarrier(1, &uav);
    uav.UAV.pResource = slot.ui_color.Get();
    list->ResourceBarrier(1, &uav);
    const auto readable = Transition(slot.ui_alpha.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(1, &readable);
    slot.alpha_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto display_readable = Transition(
        slot.display_hudless.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(1, &display_readable);
    slot.display_hudless_state =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto ui_color_readable = Transition(
        slot.ui_color.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(1, &ui_color_readable);
    slot.ui_color_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (trace)
      Log(reshade::log::level::info, "UI-mask trace: post-dispatch barriers recorded");
    hudless_tag = slot.display_hudless_sl;
    alpha_tag = slot.alpha_sl;
    ui_color_tag = slot.ui_color_sl;
    tag_extent = slot.extent;
  }
  if (trace)
    Log(reshade::log::level::info, "UI-mask trace: calling slSetTag");

  if (g_set_tag != nullptr) {
    std::array<sl::ResourceTag, 3> tags = {
        sl::ResourceTag(&hudless_tag, sl::kBufferTypeHUDLessColor,
                        sl::ResourceLifecycle::eValidUntilPresent, &tag_extent),
        sl::ResourceTag(&ui_color_tag, sl::kBufferTypeUIColorAndAlpha,
                        sl::ResourceLifecycle::eValidUntilPresent, &tag_extent),
        // The UI alpha mask also tells DLSS-G not to warp protected HUD pixels.
        // UI recomposition alone can still spatially move UI samples when the
        // game's scene motion vectors overlap a transparent instrument panel.
        sl::ResourceTag(&alpha_tag, sl::kBufferTypeNoWarpMask,
                        sl::ResourceLifecycle::eValidUntilPresent, &tag_extent)};
    const sl::Result result = g_set_tag(
        sl::ViewportHandle{0}, tags.data(), static_cast<uint32_t>(tags.size()),
        reinterpret_cast<sl::CommandBuffer*>(list));
    if (trace)
      Log(reshade::log::level::info, "UI-mask trace: slSetTag returned");
    if (result == sl::Result::eOk) {
      if (g_tagged_frames.fetch_add(1) == 0)
        Log(reshade::log::level::info,
            "submitted the first HUDLessColor + UIColorAndAlpha + NoWarpMask set to Streamline without detours");
    } else if (g_failures.fetch_add(1) < 4) {
      std::ostringstream message;
      message << "slSetTag rejected the HUDless/UI pair with result "
              << static_cast<int32_t>(result);
      Log(reshade::log::level::warning, message.str());
    }
  }
  g_ready_slot.store(index, std::memory_order_release);
  if (g_mask_frames.fetch_add(1) == 0) {
    std::ostringstream message;
    message << "generated the first native-resolution UI alpha mask at " << reason;
    Log(reshade::log::level::info, message.str());
  }
  return true;
}

bool PublishHudless(reshade::api::command_list* command, uint32_t index,
                    const char* reason) {
  if (command == nullptr || index >= kSlotCount || g_set_tag == nullptr)
    return false;
  auto* list = reinterpret_cast<ID3D12GraphicsCommandList*>(command->get_native());
  if (list == nullptr) return false;

  sl::Resource hudless{};
  sl::Extent extent{};
  {
    std::lock_guard lock(g_mutex);
    if (!g_pool.ready || g_pool.slots[index].hudless == nullptr) return false;
    hudless = g_pool.slots[index].hudless_sl;
    extent = g_pool.slots[index].extent;
  }

  const sl::ResourceTag tag(
      &hudless, sl::kBufferTypeHUDLessColor,
      sl::ResourceLifecycle::eValidUntilPresent, &extent);
  const sl::Result result = g_set_tag(
      sl::ViewportHandle{0}, &tag, 1,
      reinterpret_cast<sl::CommandBuffer*>(list));
  if (result != sl::Result::eOk) {
    if (g_failures.fetch_add(1) < 8) {
      std::ostringstream message;
      message << "slSetTag rejected HUDLessColor at " << reason
              << " with result " << static_cast<int32_t>(result);
      Log(reshade::log::level::warning, message.str());
    }
    return false;
  }

  g_ready_slot.store(index, std::memory_order_release);
  if (g_tagged_frames.fetch_add(1) == 0) {
    std::ostringstream message;
    message << "submitted the first native 4K HUDLessColor tag at " << reason;
    Log(reshade::log::level::info, message.str());
  }
  return true;
}

bool GenerateMaskAndPublishOwned(reshade::api::command_queue* queue,
                                 uint32_t index) {
  static std::atomic_bool trace_first_submission = true;
  const bool trace = trace_first_submission.exchange(false);
  if (trace) Log(reshade::log::level::info, "owned UI-mask trace: enter");
  if (queue == nullptr || index >= kSlotCount || g_set_tag == nullptr) {
    static std::atomic_bool logged_invalid_input = false;
    if (!logged_invalid_input.exchange(true))
      Log(reshade::log::level::warning,
          "owned UI-mask path rejected invalid queue, slot, or Streamline export");
    return false;
  }
  auto* native_queue = reinterpret_cast<ID3D12CommandQueue*>(queue->get_native());
  if (native_queue == nullptr) {
    static std::atomic_bool logged_missing_native_queue = false;
    if (!logged_missing_native_queue.exchange(true))
      Log(reshade::log::level::warning,
          "owned UI-mask path could not resolve the native graphics queue");
    return false;
  }

  std::lock_guard lock(g_mutex);
  if (trace) Log(reshade::log::level::info, "owned UI-mask trace: mutex acquired");
  if (!g_pool.ready || g_pool.mask_fence == nullptr) {
    static std::atomic_bool logged_missing_pool = false;
    if (!logged_missing_pool.exchange(true))
      Log(reshade::log::level::warning,
          "owned UI-mask path reached Present before its resource pool was ready");
    return false;
  }
  Slot& slot = g_pool.slots[index];
  MaskCommandContext& context = g_pool.mask_commands[index];
  if (context.fence_value != 0 &&
      g_pool.mask_fence->GetCompletedValue() < context.fence_value) {
    static std::atomic_uint32_t busy_count = 0;
    if (busy_count.fetch_add(1) < 8)
      Log(reshade::log::level::warning,
          "owned UI-mask command slot is still busy; skipped one frame");
    return false;
  }
  // Reset itself is observable by ReShade.  Install the re-entrancy guard
  // before touching the command list, not merely before recording commands.
  InternalCommandGuard internal_commands;
  if (trace) Log(reshade::log::level::info, "owned UI-mask trace: resetting command list");
  if (FAILED(context.allocator->Reset()) ||
      FAILED(context.list->Reset(context.allocator.Get(),
                                 g_pool.mask_pipeline.Get()))) {
    if (g_failures.fetch_add(1) < 8)
      Log(reshade::log::level::error,
          "could not reset the owned UI-mask command list");
    return false;
  }
  if (trace) Log(reshade::log::level::info, "owned UI-mask trace: reset complete");

  if (slot.alpha_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
    const auto barrier = Transition(slot.ui_alpha.Get(), slot.alpha_state,
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    context.list->ResourceBarrier(1, &barrier);
    slot.alpha_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  }
  if (slot.display_hudless_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
    const auto barrier = Transition(
        slot.display_hudless.Get(), slot.display_hudless_state,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    context.list->ResourceBarrier(1, &barrier);
    slot.display_hudless_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  }
  if (slot.ui_color_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
    const auto barrier = Transition(
        slot.ui_color.Get(), slot.ui_color_state,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    context.list->ResourceBarrier(1, &barrier);
    slot.ui_color_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  }
  ID3D12DescriptorHeap* heaps[] = {g_pool.descriptors.Get()};
  context.list->SetDescriptorHeaps(1, heaps);
  context.list->SetComputeRootSignature(g_pool.root_signature.Get());
  context.list->SetPipelineState(g_pool.mask_pipeline.Get());
  D3D12_GPU_DESCRIPTOR_HANDLE table =
      g_pool.descriptors->GetGPUDescriptorHandleForHeapStart();
  table.ptr += static_cast<UINT64>(index * 6) * g_pool.descriptor_size;
  context.list->SetComputeRootDescriptorTable(0, table);
  struct Constants { uint32_t width, height; float threshold, gain; } settings{
      static_cast<uint32_t>(g_pool.color_desc.Width), g_pool.color_desc.Height,
      g_threshold.load(std::memory_order_relaxed),
      g_gain.load(std::memory_order_relaxed)};
  context.list->SetComputeRoot32BitConstants(1, 4, &settings, 0);
  context.list->Dispatch((settings.width + 7) / 8,
                         (settings.height + 7) / 8, 1);
  if (trace) Log(reshade::log::level::info, "owned UI-mask trace: dispatch recorded");
  D3D12_RESOURCE_BARRIER uav{};
  uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  uav.UAV.pResource = slot.ui_alpha.Get();
  context.list->ResourceBarrier(1, &uav);
  uav.UAV.pResource = slot.display_hudless.Get();
  context.list->ResourceBarrier(1, &uav);
  uav.UAV.pResource = slot.ui_color.Get();
  context.list->ResourceBarrier(1, &uav);
  const auto readable = Transition(
      slot.ui_alpha.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  context.list->ResourceBarrier(1, &readable);
  slot.alpha_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  const auto display_readable = Transition(
      slot.display_hudless.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  context.list->ResourceBarrier(1, &display_readable);
  slot.display_hudless_state =
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  const auto ui_color_readable = Transition(
      slot.ui_color.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  context.list->ResourceBarrier(1, &ui_color_readable);
  slot.ui_color_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

  std::array<sl::ResourceTag, 3> tags = {
      sl::ResourceTag(&slot.display_hudless_sl, sl::kBufferTypeHUDLessColor,
                      sl::ResourceLifecycle::eValidUntilPresent, &slot.extent),
      sl::ResourceTag(&slot.ui_color_sl, sl::kBufferTypeUIColorAndAlpha,
                      sl::ResourceLifecycle::eValidUntilPresent, &slot.extent),
      sl::ResourceTag(&slot.alpha_sl, sl::kBufferTypeNoWarpMask,
                      sl::ResourceLifecycle::eValidUntilPresent, &slot.extent)};
  if (trace) Log(reshade::log::level::info, "owned UI-mask trace: calling slSetTag");
  const sl::Result tag_result = g_set_tag(
      sl::ViewportHandle{0}, tags.data(), static_cast<uint32_t>(tags.size()),
      reinterpret_cast<sl::CommandBuffer*>(context.list.Get()));
  if (trace) Log(reshade::log::level::info, "owned UI-mask trace: slSetTag returned");
  const HRESULT close_result = context.list->Close();
  if (trace) Log(reshade::log::level::info, "owned UI-mask trace: command list closed");
  if (tag_result != sl::Result::eOk || FAILED(close_result)) {
    if (g_failures.fetch_add(1) < 8) {
      std::ostringstream message;
      message << "owned UI-mask submission preparation failed: tag="
              << static_cast<int32_t>(tag_result)
              << " close=0x" << std::hex << static_cast<uint32_t>(close_result);
      Log(reshade::log::level::error, message.str());
    }
    return false;
  }

  ID3D12CommandList* lists[] = {context.list.Get()};
  native_queue->ExecuteCommandLists(1, lists);
  if (trace) Log(reshade::log::level::info, "owned UI-mask trace: command list executed");
  const uint64_t fence_value = g_pool.next_fence_value++;
  if (FAILED(native_queue->Signal(g_pool.mask_fence.Get(), fence_value))) {
    if (g_failures.fetch_add(1) < 8)
      Log(reshade::log::level::error,
          "could not signal the owned UI-mask command list fence");
    return false;
  }
  if (trace) Log(reshade::log::level::info, "owned UI-mask trace: fence signaled");
  context.fence_value = fence_value;
  g_ready_slot.store(index, std::memory_order_release);
  if (g_mask_frames.fetch_add(1) == 0)
    Log(reshade::log::level::info,
        "submitted the first owned native-4K HUDLessColor + UIColorAndAlpha + NoWarpMask pass");
  return true;
}

void OnCloseCommandList(reshade::api::command_list* command) {
  if (g_recording_internal_commands) return;
  FinalizeClassicUiSequence(command, "command-list-close");
  uint32_t slot = UINT32_MAX;
  bool should_prepare = false;
  {
    std::lock_guard lock(g_mutex);
    const auto found = g_commands.find(CommandKey(command));
    if (found != g_commands.end()) {
      slot = found->second.slot;
      should_prepare = found->second.post_captured && slot < kSlotCount;
    }
  }
  // The pair is now complete, but GPU mask work is deliberately deferred to
  // Present.  OnExecuteCommandList publishes the slot only after the game's
  // capture command list has entered the graphics queue.
  (void)should_prepare;
  (void)slot;
}

void OnExecuteCommandList(reshade::api::command_queue*,
                          reshade::api::command_list* command) {
  if (g_recording_internal_commands) return;
  std::lock_guard lock(g_mutex);
  auto found = g_commands.find(CommandKey(command));
  if (found != g_commands.end() && found->second.post_captured &&
      found->second.slot < kSlotCount) {
    g_submitted_slot.store(found->second.slot, std::memory_order_release);
    found->second.post_captured = false;
    const uint32_t debug_index = g_debug_execute_count.fetch_add(1);
    if (debug_index < 16) {
      std::ostringstream message;
      message << "execute_command_list published slot=" << found->second.slot
              << " command=0x" << std::hex << CommandKey(command);
      Log(reshade::log::level::info, message.str());
    }
  }
}

void OnPresent(reshade::api::command_queue* queue, reshade::api::swapchain* swapchain,
               const reshade::api::rect*, const reshade::api::rect*, uint32_t,
               const reshade::api::rect*) {
  // Streamline can be loaded after the add-on and after swap-chain creation.
  // Retry here instead of relying on a single startup-time observation.
  TryInstallHooks();
  // The native DLSS-G provider is loaded even later than Streamline in
  // Motorsport. Retry this independent hook as well; previously the function
  // existed but no live path ever called it, so NGX kept IGNORE_UI_TEXTURE.
  // Do not Detour the OTA model image's exported EvaluateFeature entry after
  // it is live: this provider uses an internal dispatch layout for which a
  // late prologue detour blackens presentation. UI separation stays on the
  // supported Streamline tag/options path above.
  CompleteMotionCapture(queue);
  (void)swapchain;
}

void OnRenderTechnique(reshade::api::effect_runtime* runtime,
                       reshade::api::effect_technique,
                       reshade::api::command_list* command,
                       reshade::api::resource_view rtv,
                       reshade::api::resource_view) {
  // ReShade owns this command list and guarantees that it is open while this
  // callback runs.  This event is emitted after the carrier's no-op compute
  // technique, so no render pass is active, but before ReShade closes and
  // submits the command list.  In particular, do not request
  // the immediate list from inside Present and do not submit a raw native
  // command list: both paths violate Motorsport's ReShade/Streamline proxy
  // ownership and crash on the first frame containing the HUD.
  uint32_t index =
      g_submitted_slot.exchange(UINT32_MAX, std::memory_order_acq_rel);
  const uint32_t debug_index = g_debug_present_count.fetch_add(1);
  if (debug_index < 32 || index < kSlotCount) {
    std::ostringstream message;
    message << "reshade_render_technique #" << debug_index
            << " slot=" << index
            << " command=0x" << std::hex << CommandKey(command);
    Log(reshade::log::level::info, message.str());
  }
  ID3D12Resource* final_color = nullptr;
  if (runtime != nullptr && rtv.handle != 0) {
    const reshade::api::resource resource =
        runtime->get_device()->get_resource_from_view(rtv);
    final_color = reinterpret_cast<ID3D12Resource*>(resource.handle);
  }
  // Current Motorsport builds no longer emit the HUD draw/render-pass events
  // used to associate a capture slot with this ReShade command list.  Do not
  // silently skip UI recomposition when that game-side signal is absent.
  // Build a slot directly from the native 4K output and rotate it once per
  // real frame.  The shader publishes complete opaque protected HUD zones, so
  // this path needs no pre/post-HUD scene captures.
  if (index >= kSlotCount && final_color != nullptr) {
    auto* native = reinterpret_cast<ID3D12GraphicsCommandList*>(command->get_native());
    if (EnsurePool(final_color, native)) {
      index = g_fallback_slot_counter.fetch_add(1, std::memory_order_relaxed) %
              kSlotCount;
      static std::atomic_bool logged_fallback{false};
      if (!logged_fallback.exchange(true))
        Log(reshade::log::level::warning,
            "HUD callbacks unavailable; using final-4K opaque-zone UI recomposition fallback");
    }
  }
  if (index < kSlotCount)
    GenerateMaskAndPublish(command, index, "reshade-render-technique",
                           final_color);
  RecordMotionCapture(command);
}

// ---- Native DLSS-G NGX bridge -------------------------------------------

using NgxEvaluateFn = NVSDK_NGX_Result(NVSDK_CONV*)(
    ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
    const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);

NgxEvaluateFn g_ngx_evaluate = nullptr;
std::atomic_bool g_ngx_hooked{false};
std::atomic_bool g_ngx_inputs_logged{false};
std::atomic_bool g_ngx_dispatch_flags_logged{false};
std::atomic_bool g_ngx_call_logged{false};
std::vector<mfgunlock::hook::HookItem> g_ngx_hooks;

HMODULE FindActiveDlssgProvider() {
  std::array<HMODULE, 1024> modules{};
  DWORD bytes_needed = 0;
  if (K32EnumProcessModules(GetCurrentProcess(), modules.data(),
                            static_cast<DWORD>(sizeof(modules)),
                            &bytes_needed)) {
    const size_t count = (std::min)(modules.size(),
        static_cast<size_t>(bytes_needed / sizeof(HMODULE)));
    HMODULE local_provider = nullptr;
    for (size_t index = 0; index < count; ++index) {
      wchar_t path[32768]{};
      if (GetModuleFileNameW(modules[index], path,
                             static_cast<DWORD>(std::size(path))) == 0)
        continue;
      std::wstring lowered(path);
      std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                     [](wchar_t value) {
                       return static_cast<wchar_t>(std::towlower(value));
                     });
      // The current NVIDIA app/NGX OTA path maps the real DLSS-G provider as
      // a PE image named 160_E658700.bin. DLSS SR maps another module with the
      // same basename, so the full models\\dlssg path is the only safe key.
      if (lowered.find(L"\\models\\dlssg\\") != std::wstring::npos &&
          GetProcAddress(modules[index],
                         "NVSDK_NGX_D3D12_EvaluateFeature") != nullptr)
        return modules[index];
      if (lowered.find(L"nvngx_dlssg") != std::wstring::npos &&
          GetProcAddress(modules[index],
                         "NVSDK_NGX_D3D12_EvaluateFeature") != nullptr)
        local_provider = modules[index];
    }
    if (local_provider != nullptr) return local_provider;
  }
  return GetModuleHandleW(L"nvngx_dlssg.dll");
}

NVSDK_NGX_Result NVSDK_CONV HookNgxEvaluate(
    ID3D12GraphicsCommandList* command, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters,
    PFN_NVSDK_NGX_ProgressCallback callback) {
  if (g_ngx_evaluate == nullptr || parameters == nullptr)
    return NVSDK_NGX_Result_FAIL_InvalidParameter;

  const bool trace_first_call = !g_ngx_call_logged.exchange(true);
  if (trace_first_call) {
    std::ostringstream message;
    message << "entered native NGX EvaluateFeature command=" << command
            << " handle=" << handle << " parameters=" << parameters
            << " original=" << reinterpret_cast<void*>(g_ngx_evaluate);
    Log(reshade::log::level::info, message.str());
  }

  const uint32_t slot_index = g_ready_slot.load(std::memory_order_acquire);
  ID3D12Resource* backbuffer = nullptr;
  parameters->Get("DLSSG.Backbuffer", &backbuffer);
  bool injected = false;
  if (slot_index < kSlotCount && backbuffer != nullptr) {
    std::lock_guard lock(g_mutex);
    if (g_pool.ready) {
      Slot& slot = g_pool.slots[slot_index];
      const D3D12_RESOURCE_DESC actual = backbuffer->GetDesc();
      const bool same_output = actual.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
          actual.Width == g_pool.color_desc.Width &&
          actual.Height == g_pool.color_desc.Height;
      if (same_output && slot.display_hudless != nullptr &&
          slot.ui_alpha != nullptr) {
        auto* writable = const_cast<NVSDK_NGX_Parameter*>(parameters);
        writable->Set("DLSSG.HUDLess", slot.display_hudless.Get());
        writable->Set("DLSSG.UI", slot.ui_color.Get());

        // OptiScaler's NVNGX frame-generation proxy defaults this bit to on.
        // It explicitly tells the provider to ignore DLSSG.UI, which defeats
        // the per-frame HUDless + premultiplied UI pair above and causes the
        // HUD to be interpolated as part of the scene. Clear only that bit on
        // this Motorsport evaluation; keep every other provider flag intact.
        constexpr uint32_t kIgnoreUiTexture = 0x10000000u;
        uint32_t dispatch_flags = 0;
        const NVSDK_NGX_Result flags_result =
            parameters->Get("DLSSG.DispatchFlags", &dispatch_flags);
        if (flags_result == NVSDK_NGX_Result_Success) {
          const uint32_t corrected_flags = dispatch_flags & ~kIgnoreUiTexture;
          writable->Set("DLSSG.DispatchFlags", corrected_flags);
          if (!g_ngx_dispatch_flags_logged.exchange(true)) {
            std::ostringstream message;
            message << "DLSSG dispatch flags 0x" << std::hex << dispatch_flags
                    << " -> 0x" << corrected_flags
                    << " (cleared IGNORE_UI_TEXTURE for native UI recomposition)";
            Log(reshade::log::level::info, message.str());
          }
        } else if (!g_ngx_dispatch_flags_logged.exchange(true)) {
          Log(reshade::log::level::warning,
              "DLSSG.DispatchFlags was unavailable; dedicated UI texture could not be unmasked");
        }
        injected = true;
      }
    }
  }
  if (injected && !g_ngx_inputs_logged.exchange(true))
    Log(reshade::log::level::info,
        "injected the first display-encoded 4K HUDLess + premultiplied RGBA UI pair directly into DLSS-G NGX evaluation");
  const NVSDK_NGX_Result result =
      g_ngx_evaluate(command, handle, parameters, callback);
  if (trace_first_call) {
    std::ostringstream message;
    message << "first native NGX EvaluateFeature returned 0x" << std::hex
            << static_cast<uint32_t>(result) << " injected=" << std::dec
            << (injected ? 1 : 0) << " backbuffer=" << backbuffer;
    Log(reshade::log::level::info, message.str());
  }
  return result;
}

void TryInstallNgxHook() {
  if (!g_enabled.load(std::memory_order_relaxed) || g_ngx_hooked.load()) return;
  HMODULE module = FindActiveDlssgProvider();
  if (module == nullptr ||
      GetProcAddress(module, "NVSDK_NGX_D3D12_EvaluateFeature") == nullptr)
    return;
  g_ngx_hooks = {{"NVSDK_NGX_D3D12_EvaluateFeature",
                  reinterpret_cast<void**>(&g_ngx_evaluate),
                  reinterpret_cast<void*>(&HookNgxEvaluate)}};
  if (mfgunlock::hook::Install(module, g_ngx_hooks,
                               "Forza Motorsport native DLSS-G NGX bridge")) {
    g_ngx_hooked.store(true, std::memory_order_release);
    wchar_t path[32768]{};
    GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
    Log(reshade::log::level::info,
        "native DLSS-G NGX evaluation hook installed on " +
            std::string(path, path + std::wcslen(path)));
  }
}

// ---- Streamline bridge ----------------------------------------------------

PFun_slGetFeatureFunction* g_get_function = nullptr;
PFun_slDLSSGSetOptions* g_set_options = nullptr;

bool BuildUiOptions(const sl::DLSSGOptions& source, sl::DLSSGOptions& result) {
  if (source.structVersion < sl::kStructVersion1 ||
      source.structVersion > sl::kStructVersion5) return false;
  result = sl::DLSSGOptions{};
  result.next = source.next;
  result.structVersion = (std::max)(size_t{sl::kStructVersion4}, source.structVersion);
  result.mode = source.mode;
  result.numFramesToGenerate = source.numFramesToGenerate;
  result.flags = source.flags;
  result.dynamicResWidth = source.dynamicResWidth;
  result.dynamicResHeight = source.dynamicResHeight;
  result.numBackBuffers = source.numBackBuffers;
  result.mvecDepthWidth = source.mvecDepthWidth;
  result.mvecDepthHeight = source.mvecDepthHeight;
  result.colorWidth = source.colorWidth;
  result.colorHeight = source.colorHeight;
  result.colorBufferFormat = source.colorBufferFormat;
  result.mvecBufferFormat = source.mvecBufferFormat;
  result.depthBufferFormat = source.depthBufferFormat;
  {
    std::lock_guard lock(g_mutex);
    if (!g_pool.ready) return false;
    result.hudLessBufferFormat =
        static_cast<uint32_t>(DXGI_FORMAT_R10G10B10A2_UNORM);
  }
  result.uiBufferFormat =
      static_cast<uint32_t>(DXGI_FORMAT_R16G16B16A16_UNORM);
  if (result.colorBufferFormat == 0)
    result.colorBufferFormat = static_cast<uint32_t>(g_backbuffer_format);
  result.onErrorCallback = source.onErrorCallback;
  if (source.structVersion >= sl::kStructVersion2) result.bReserved15 = source.bReserved15;
  if (source.structVersion >= sl::kStructVersion3)
    result.queueParallelismMode = source.queueParallelismMode;
  result.enableUserInterfaceRecomposition = sl::eTrue;
  if (source.structVersion >= sl::kStructVersion5)
    result.dynamicTargetFrameRate = source.dynamicTargetFrameRate;
  return true;
}

sl::Result HookSetOptions(const sl::ViewportHandle& viewport,
                          const sl::DLSSGOptions& options) {
  if (!g_enabled.load(std::memory_order_relaxed) ||
      options.mode == sl::DLSSGMode::eOff) return g_set_options(viewport, options);
  sl::DLSSGOptions advanced{};
  if (!BuildUiOptions(options, advanced)) return g_set_options(viewport, options);
  const sl::Result result = g_set_options(viewport, advanced);
  if (result == sl::Result::eOk) return result;
  ++g_failures;
  Log(reshade::log::level::warning,
      "Streamline rejected UI recomposition; falling back to the game's options");
  return g_set_options(viewport, options);
}

bool HasScalingOutput(const sl::ResourceTag* tags, uint32_t count) {
  if (tags == nullptr) return false;
  for (uint32_t index = 0; index < count; ++index)
    if (tags[index].type == sl::kBufferTypeScalingOutputColor &&
        tags[index].resource != nullptr) return true;
  return false;
}

template <typename Forward>
sl::Result ForwardTags(uint32_t frame, const sl::ViewportHandle& viewport,
                       const sl::ResourceTag* tags, uint32_t count,
                       sl::CommandBuffer* commands, Forward&& forward) {
  if (!g_enabled.load(std::memory_order_relaxed))
    return forward(tags, count);
  const uint64_t command_key = commands != nullptr
      ? NormalizeCommandPointer(commands, true)
      : 0;
  uint32_t slot_index = UINT32_MAX;
  Slot* slot = nullptr;
  {
    std::lock_guard lock(g_mutex);
    // Motorsport does not publish ScalingOutputColor through the accessible
    // slSetTag chain.  Bind a slot to the native command list on any tag call;
    // later UI draw callbacks populate that exact resource before Present.
    if (command_key != 0) {
      CommandState& state = g_commands[command_key];
      if (state.slot >= kSlotCount)
        state.slot = g_fallback_slot_counter.fetch_add(1, std::memory_order_relaxed) %
                     kSlotCount;
      slot_index = state.slot;
    } else {
      slot_index = frame % kSlotCount;
    }
    if (!g_pool.ready) return forward(tags, count);
    slot = &g_pool.slots[slot_index];
  }
  std::vector<sl::ResourceTag> forwarded(tags, tags + count);
  forwarded.emplace_back(&slot->display_hudless_sl,
                         sl::kBufferTypeHUDLessColor,
                         sl::ResourceLifecycle::eValidUntilPresent, &slot->extent);
  forwarded.emplace_back(&slot->ui_color_sl, sl::kBufferTypeUIColorAndAlpha,
                         sl::ResourceLifecycle::eValidUntilPresent, &slot->extent);
  forwarded.emplace_back(&slot->alpha_sl, sl::kBufferTypeNoWarpMask,
                         sl::ResourceLifecycle::eValidUntilPresent, &slot->extent);
  if (g_tagged_frames.fetch_add(1) == 0)
    Log(reshade::log::level::info,
        "submitted the first HUDLessColor + UIColorAndAlpha + NoWarpMask set to Streamline");
  return forward(forwarded.data(), static_cast<uint32_t>(forwarded.size()));
}

std::atomic<uint32_t> g_legacy_frame{0};

sl::Result HookSetTag(const sl::ViewportHandle& viewport, const sl::ResourceTag* tags,
                      uint32_t count, sl::CommandBuffer* commands) {
  if (!g_logged_first_tag_call.exchange(true))
    Log(reshade::log::level::info,
        "received the first Streamline resource-tag call through the cooperative hook chain");
  uint32_t frame = g_legacy_frame.load(std::memory_order_relaxed);
  if (HasScalingOutput(tags, count)) frame = g_legacy_frame.fetch_add(1) + 1;
  return ForwardTags(frame, viewport, tags, count, commands,
      [&](const sl::ResourceTag* data, uint32_t size) {
        return g_set_tag(viewport, data, size, commands);
      });
}

sl::Result HookSetTagForFrame(const sl::FrameToken& frame,
                              const sl::ViewportHandle& viewport,
                              const sl::ResourceTag* tags, uint32_t count,
                              sl::CommandBuffer* commands) {
  if (!g_logged_first_tag_call.exchange(true))
    Log(reshade::log::level::info,
        "received the first Streamline resource-tag call through the cooperative hook chain");
  return ForwardTags(static_cast<uint32_t>(frame), viewport, tags, count, commands,
      [&](const sl::ResourceTag* data, uint32_t size) {
        return g_set_tag_for_frame(frame, viewport, data, size, commands);
      });
}

sl::Result HookGetFunction(sl::Feature feature, const char* name, void*& function) {
  const sl::Result result = g_get_function(feature, name, function);
  if (result != sl::Result::eOk || feature != sl::kFeatureDLSS_G ||
      name == nullptr || function == nullptr) return result;
  if (std::strcmp(name, "slDLSSGSetOptions") == 0 &&
      function != reinterpret_cast<void*>(&HookSetOptions)) {
    g_set_options = reinterpret_cast<PFun_slDLSSGSetOptions*>(function);
    function = reinterpret_cast<void*>(&HookSetOptions);
  }
  return result;
}

std::vector<mfgunlock::hook::HookItem> g_sl_hooks;
SRWLOCK g_install_lock = SRWLOCK_INIT;

void TryInstallHooks() {
  if (!g_enabled.load() || g_hooks_installed.load()) return;
  if (!TryAcquireSRWLockExclusive(&g_install_lock)) return;
  HMODULE interposer = GetModuleHandleW(L"sl.interposer.dll");
  if (interposer != nullptr && !g_hooks_installed.load()) {
    g_get_native_interface = reinterpret_cast<PFun_slGetNativeInterface*>(
        GetProcAddress(interposer, "slGetNativeInterface"));
    g_sl_hooks.clear();
    // Resolve Streamline's public exports but do not detour any of them.
    // Transfusion already owns this call chain; stacking another Detours
    // trampoline corrupts Motorsport's feature-creation path. Resources are
    // submitted explicitly from the Present callback instead.
    g_set_tag = reinterpret_cast<SetTagFn>(GetProcAddress(interposer, "slSetTag"));
    g_set_tag_for_frame = reinterpret_cast<PFun_slSetTagForFrame*>(
        GetProcAddress(interposer, "slSetTagForFrame"));
    if (g_set_tag != nullptr) {
      g_hooks_installed.store(true);
      Log(reshade::log::level::info,
          "Streamline resource exports resolved (zero detours installed)");
    }
  }
  ReleaseSRWLockExclusive(&g_install_lock);
}

using LoadWFn = HMODULE (WINAPI*)(LPCWSTR);
using LoadExWFn = HMODULE (WINAPI*)(LPCWSTR, HANDLE, DWORD);
LoadWFn g_load_w = nullptr;
LoadExWFn g_load_ex_w = nullptr;

HMODULE WINAPI HookLoadW(LPCWSTR path) {
  HMODULE module = g_load_w(path);
  if (module != nullptr) TryInstallHooks();
  return module;
}

HMODULE WINAPI HookLoadExW(LPCWSTR path, HANDLE file, DWORD flags) {
  HMODULE module = g_load_ex_w(path, file, flags);
  if (module != nullptr && (flags & (LOAD_LIBRARY_AS_DATAFILE |
      LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE)) == 0)
    TryInstallHooks();
  return module;
}

const std::vector<mfgunlock::hook::HookItem> kLoaderHooks = {
    {"LoadLibraryW", reinterpret_cast<void**>(&g_load_w), reinterpret_cast<void*>(&HookLoadW)},
    {"LoadLibraryExW", reinterpret_cast<void**>(&g_load_ex_w),
     reinterpret_cast<void*>(&HookLoadExW)}};

}  // namespace

extern "C" __declspec(dllexport) constexpr const char* NAME =
    "Forza Motorsport HUDless MFG";
extern "C" __declspec(dllexport) constexpr const char* DESCRIPTION =
    "Adds display-matched HUDlessColor and premultiplied UIColorAndAlpha resources to Forza Motorsport DLSS-G";

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    if (!reshade::register_addon(module)) return FALSE;
    {
      std::ostringstream message;
      message << "loaded in process " << GetCurrentProcessId();
      Log(reshade::log::level::info, message.str());
    }
    int enabled = 1;
    reshade::get_config_value(nullptr, kConfigSection, "Enabled", enabled);
    g_enabled.store(enabled != 0);
    float threshold = g_threshold.load();
    float gain = g_gain.load();
    reshade::get_config_value(nullptr, kConfigSection, "MaskThreshold", threshold);
    reshade::get_config_value(nullptr, kConfigSection, "MaskGain", gain);
    reshade::get_config_value(nullptr, kConfigSection, "ProbeLevel", g_probe_level);
    g_probe_level = (std::max)(0, (std::min)(2, g_probe_level));
    if (threshold > 0.0f && threshold < 0.1f) g_threshold.store(threshold);
    if (gain >= 0.0f && gain <= 2048.0f) g_gain.store(gain);
    if (g_enabled.load()) {
      reshade::register_event<reshade::addon_event::init_swapchain>(OnInitSwapchain);
      if (g_probe_level >= 1) {
        reshade::register_event<reshade::addon_event::init_pipeline>(OnInitPipeline);
        reshade::register_event<reshade::addon_event::destroy_pipeline>(OnDestroyPipeline);
      }
      if (g_probe_level >= 2) {
        reshade::register_event<reshade::addon_event::reset_command_list>(OnResetCommandList);
        reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(OnBindTargets);
        reshade::register_event<reshade::addon_event::begin_render_pass>(OnBeginRenderPass);
        reshade::register_event<reshade::addon_event::bind_pipeline>(OnBindPipeline);
        reshade::register_event<reshade::addon_event::draw>(OnDraw);
        reshade::register_event<reshade::addon_event::draw_indexed>(OnDrawIndexed);
        reshade::register_event<reshade::addon_event::end_render_pass>(OnEndRenderPass);
        reshade::register_event<reshade::addon_event::close_command_list>(OnCloseCommandList);
        reshade::register_event<reshade::addon_event::execute_command_list>(OnExecuteCommandList);
      }
      reshade::register_event<reshade::addon_event::present>(OnPresent);
      reshade::register_event<reshade::addon_event::reshade_render_technique>(OnRenderTechnique);
    }
  } else if (reason == DLL_PROCESS_DETACH) {
    if (g_loader_hooked.load()) mfgunlock::hook::Uninstall(kLoaderHooks);
    if (!g_sl_hooks.empty()) mfgunlock::hook::Uninstall(g_sl_hooks);
    if (g_ngx_hooked.load()) mfgunlock::hook::Uninstall(g_ngx_hooks);
    reshade::unregister_addon(module);
  }
  return TRUE;
}
