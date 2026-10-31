// Komiho: QNN/HTP (Qualcomm NPU) backend, ported verbatim from
// HaoweiLi97/mihon_img_upscale (Apache-2.0), minus the spatial-depth parts.
// MIHON_ENABLE_QNN=0 compiles to stubs (probe fails, process returns -1).
#include "qnn_backend.h"
#include "upscale_pixels.h"
#include "native_log.h"

#include <algorithm>
#include <android/log.h>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <string>
#include <vector>
#include <thread>

#if MIHON_ENABLE_QNN
#include "qnn_power.h"
#include "qnn_shared_io.h"
#include <HTP/QnnHtpDevice.h>
#include <QnnInterface.h>
#include <System/QnnSystemInterface.h>
#endif

#define TAG "QnnBackend"
#define LOGD(...) mihonsy_native_log(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGW(...) mihonsy_native_log(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) mihonsy_native_log(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace qnn_backend {

static std::atomic<int> thermal_status{0};
void set_thermal_status(int status) { thermal_status.store(std::clamp(status, 0, 6)); }

// ── Komiho (2026-09-18): ADSP_LIBRARY_PATH 必须在「首次 dlopen libQnnHtp.so」之前就位 ──
//
// 为什么值得为它单独写一节：真机日志（run 35263307970 的装机版）的时间顺序是
//     43.157  QnnBackend: Detected HTP architecture v75 from QNN platform info
//                                                            ← architecture() 已经 dlopen 过
//     43.163  Waifu2xJNI: QNN ADSP_LIBRARY_PATH=...           ← 我们到这一刻才 setenv
//     43.171  QnnDsp <I> QnnLog_create started.               ← 才真正 initialize
// 也就是说 libQnnHtp.so 的**第一次映射**（以及它内部 FastRPC router、DSP 会话的建立）
// 发生在一个「ADSP_LIBRARY_PATH 还没设」的进程环境里。此后 DSP 侧就再也装不上 Skel：
//     loadRemoteSymbols failed with err 4000 → Failed to create transport
//     → Failed to load skel, error: 4000 → Transport layer setup failed: 14001
// 而且**之后每一次重试都失败**（进程级的 FastRPC/DSP 状态一旦建错就不会自愈）。
//
// 更要命的是：waifu2x_jni.cpp 顶部的注释本来就写着「ADSP_LIBRARY_PATH 必须在首次
// dlopen 前指向本 App 的 nativeLibraryDir」—— 注释与实现自相矛盾，这里把它改成事实：
//   * 调用点：JNI_OnLoad（= Kotlin System.loadLibrary("waifu2x-jni") 那一刻），
//     以及每个 QNN 入口函数的最前面（幂等，兜底）。
//   * 路径不再依赖 Kotlin 传参：优先用调用方给的 nativeLibraryDir，没有就用 dladdr
//     自定位本 .so 所在目录（即 …/lib/arm64）。
//
// 为什么路径是这几段：HTP Skel 由 DSP 侧的加载器按 ADSP_LIBRARY_PATH 逐段查找，
// 第一段必须放我们自己的 lib 目录（Skel/Stub 就在那儿），后面三段是参照实现同款的
// vendor 兜底目录（字面量与上游 libwaifu2x-jni.so 内完全一致）。
namespace {
std::string own_library_dir() {
  Dl_info info{};
  if (dladdr(reinterpret_cast<void *>(&own_library_dir), &info) != 0 &&
      info.dli_fname) {
    const std::string full(info.dli_fname);
    const size_t slash = full.find_last_of('/');
    if (slash != std::string::npos) {
      return full.substr(0, slash);
    }
  }
  return {};
}
} // namespace

void ensure_dsp_path(const char *preferred_dir) {
  const std::string dir = (preferred_dir && *preferred_dir)
                              ? std::string(preferred_dir)
                              : own_library_dir();
  if (dir.empty()) {
    LOGW("Unable to locate the native library dir; ADSP_LIBRARY_PATH untouched");
    return;
  }
  const std::string paths =
      dir + ";/vendor/dsp/cdsp;/vendor/lib/rfsa/cdsp;/system/vendor/lib/rfsa/cdsp";
  const char *before = getenv("ADSP_LIBRARY_PATH");
  LOGD("DSP path: before=\"%s\"", before ? before : "(unset)");
  setenv("ADSP_LIBRARY_PATH", paths.c_str(), 1);
  LOGD("QNN ADSP_LIBRARY_PATH=%s", paths.c_str());
}

#if MIHON_ENABLE_QNN
namespace {

using GetProvidersFn = Qnn_ErrorHandle_t (*)(const QnnInterface_t ***, uint32_t *);
using GetSystemProvidersFn =
    Qnn_ErrorHandle_t (*)(const QnnSystemInterface_t ***, uint32_t *);

// Komiho (2026-09-18, diagnostic — safe to delete once NPU is confirmed).
//
// Why this exists: passing `nullptr` as the logger to QnnBackend_create() makes QNN
// use its own default logger, which only emits ERROR and above. That hides the one
// line that names the HTP skel the backend is trying to load:
//   QnnDsp <V> Attempting to open dynamically linked so: <name> using base filename
// Without it we cannot tell *which* arch it asks for, and we have been reduced to
// guessing why `Failed to load skel, error: 4000` happens even though a byte-identical
// libQnnHtpV75Skel.so sits in the app's lib/arm64 directory.
// We therefore install our own logger at DEBUG level (the most verbose level the
// header exposes) for the init path only, then drop it back to ERROR so the
// inference loop stays quiet.
void qnn_log_callback(const char *fmt, QnnLog_Level_t level,
                      uint64_t /*timestamp*/, va_list args) {
  int priority;
  switch (level) {
    case QNN_LOG_LEVEL_ERROR:
      priority = ANDROID_LOG_ERROR;
      break;
    case QNN_LOG_LEVEL_WARN:
      priority = ANDROID_LOG_WARN;
      break;
    case QNN_LOG_LEVEL_INFO:
      priority = ANDROID_LOG_INFO;
      break;
    case QNN_LOG_LEVEL_VERBOSE:
      priority = ANDROID_LOG_VERBOSE;
      break;
    case QNN_LOG_LEVEL_DEBUG:
      priority = ANDROID_LOG_DEBUG;
      break;
    default:
      priority = ANDROID_LOG_DEFAULT;
      break;
  }
  __android_log_vprint(priority, "QnnHost", fmt, args);
}

struct GraphMetadata {
  const char *name = nullptr;
  const Qnn_Tensor_t *inputs = nullptr;
  uint32_t input_count = 0;
  const Qnn_Tensor_t *outputs = nullptr;
  uint32_t output_count = 0;
};

bool get_graph_metadata(const QnnSystemContext_BinaryInfo_t *binary_info,
                        GraphMetadata &metadata) {
  if (!binary_info) {
    return false;
  }

  const QnnSystemContext_GraphInfo_t *graphs = nullptr;
  uint32_t graph_count = 0;
  switch (binary_info->version) {
  case QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_1:
    graphs = binary_info->contextBinaryInfoV1.graphs;
    graph_count = binary_info->contextBinaryInfoV1.numGraphs;
    break;
  case QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_2:
    graphs = binary_info->contextBinaryInfoV2.graphs;
    graph_count = binary_info->contextBinaryInfoV2.numGraphs;
    break;
  case QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_3:
    graphs = binary_info->contextBinaryInfoV3.graphs;
    graph_count = binary_info->contextBinaryInfoV3.numGraphs;
    break;
  default:
    // Komiho: say *why* instead of failing silently — a newer BinaryInfo layout would
    // otherwise surface as a bare "graph metadata unusable".
    LOGE("Unsupported BinaryInfo version: %u",
         static_cast<unsigned>(binary_info->version));
    return false;
  }
  if (!graphs || graph_count != 1) {
    LOGE("Expected one graph in context, found %u", graph_count);
    return false;
  }

  const auto &graph = graphs[0];
  switch (graph.version) {
  case QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_1:
    metadata = {graph.graphInfoV1.graphName, graph.graphInfoV1.graphInputs,
                graph.graphInfoV1.numGraphInputs,
                graph.graphInfoV1.graphOutputs,
                graph.graphInfoV1.numGraphOutputs};
    break;
  case QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_2:
    metadata = {graph.graphInfoV2.graphName, graph.graphInfoV2.graphInputs,
                graph.graphInfoV2.numGraphInputs,
                graph.graphInfoV2.graphOutputs,
                graph.graphInfoV2.numGraphOutputs};
    break;
  case QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_3:
    metadata = {graph.graphInfoV3.graphName, graph.graphInfoV3.graphInputs,
                graph.graphInfoV3.numGraphInputs,
                graph.graphInfoV3.graphOutputs,
                graph.graphInfoV3.numGraphOutputs};
    break;
  default:
    LOGE("Unsupported GraphInfo version: %u", static_cast<unsigned>(graph.version));
    return false;
  }
  return metadata.name && metadata.inputs && metadata.outputs &&
         metadata.input_count == 1 && metadata.output_count == 1;
}

uint16_t float_to_half(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  const uint32_t sign = (bits >> 16) & 0x8000u;
  uint32_t mantissa = bits & 0x007fffffu;
  int exponent = static_cast<int>((bits >> 23) & 0xffu) - 127 + 15;

  if (exponent <= 0) {
    if (exponent < -10) {
      return static_cast<uint16_t>(sign);
    }
    mantissa = (mantissa | 0x00800000u) >> (1 - exponent);
    return static_cast<uint16_t>(sign + ((mantissa + 0x00001000u) >> 13));
  }
  if (exponent >= 31) {
    return static_cast<uint16_t>(sign | 0x7c00u);
  }
  return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent) << 10) |
                               ((mantissa + 0x00001000u) >> 13));
}

using upscale_pixels::half_to_float;

int reflect_coordinate(int coordinate, int size) {
  if (size <= 1) {
    return 0;
  }
  while (coordinate < 0 || coordinate >= size) {
    coordinate = coordinate < 0 ? -coordinate : 2 * size - 2 - coordinate;
  }
  return coordinate;
}

class Runtime {
public:
  ~Runtime() { reset(); }

  bool probe() {
    ensure_dsp_path(nullptr); // 必须在 dlopen 之前
    void *handle = dlopen("libQnnHtp.so", RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
      LOGE("Unable to load libQnnHtp.so: %s", dlerror());
      return false;
    }
    const bool result = find_qnn_provider(handle) != nullptr;
    dlclose(handle);
    return result;
  }

  int architecture() {
    ensure_dsp_path(nullptr); // 必须在 dlopen 之前（首次调用尤其关键）
    void *handle = dlopen("libQnnHtp.so", RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
      LOGE("Unable to load libQnnHtp.so while detecting architecture: %s",
           dlerror());
      return 0;
    }
    const QnnInterface_t *provider = find_qnn_provider(handle);
    if (!provider) {
      dlclose(handle);
      return 0;
    }
    const auto &qnn = provider->QNN_INTERFACE_VER_NAME;
    if (!qnn.deviceGetPlatformInfo || !qnn.deviceFreePlatformInfo) {
      dlclose(handle);
      return 0;
    }

    const QnnDevice_PlatformInfo_t *platform_info = nullptr;
    int result = 0;
    if (qnn.deviceGetPlatformInfo(nullptr, &platform_info) == QNN_SUCCESS &&
        platform_info &&
        platform_info->version == QNN_DEVICE_PLATFORM_INFO_VERSION_1) {
      for (uint32_t index = 0; index < platform_info->v1.numHwDevices; ++index) {
        const auto &device = platform_info->v1.hwDevices[index];
        if (device.version != QNN_DEVICE_HARDWARE_DEVICE_INFO_VERSION_1 ||
            !device.v1.deviceInfoExtension) {
          continue;
        }
        const auto *extension =
            reinterpret_cast<const QnnHtpDevice_DeviceInfoExtension_t *>(
                device.v1.deviceInfoExtension);
        if (extension->devType == QNN_HTP_DEVICE_TYPE_ON_CHIP) {
          result = static_cast<int>(extension->onChipDevice.arch);
          break;
        }
      }
    }
    if (platform_info) {
      qnn.deviceFreePlatformInfo(nullptr, platform_info);
    }
    dlclose(handle);
    LOGD("Detected HTP architecture v%d from QNN platform info", result);
    return result;
  }

  bool load(const std::string &context_path, int padding) {
    deactivate();
    if (!ensure_runtime()) {
      return false;
    }
    const auto &qnn = provider_->QNN_INTERFACE_VER_NAME;

    std::ifstream stream(context_path, std::ios::binary | std::ios::ate);
    if (!stream) {
      LOGE("Unable to open QNN context: %s", context_path.c_str());
      return false;
    }
    const auto length = stream.tellg();
    if (length <= 0) {
      return false;
    }
    binary_.resize(static_cast<size_t>(length));
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char *>(binary_.data()), length)) {
      LOGE("Unable to read QNN context: %s", context_path.c_str());
      return false;
    }

    GraphMetadata metadata;
    const auto &system = system_provider_->QNN_SYSTEM_INTERFACE_VER_NAME;
    QnnSystemContext_Handle_t system_context = nullptr;
    const QnnSystemContext_BinaryInfo_t *binary_info = nullptr;
    Qnn_ContextBinarySize_t binary_info_size = 0;

    // Komiho (2026-09-19): each step is checked and logged separately. These used to share a
    // single `if (...) { LOGE("Unable to read QNN context graph metadata"); }`, so a
    // device-side failure printed that one line and nothing else — no way to tell whether
    // getBinaryInfo, the graph metadata or a tensor was at fault. That ambiguity cost a full
    // debugging round (the real cause was a version-1 tensor); see SKILL.md.
    bool metadata_ok = system.systemContextCreate != nullptr &&
                       system.systemContextGetBinaryInfo != nullptr &&
                       system.systemContextFree != nullptr;
    if (!metadata_ok) {
      LOGE("QNN system context API incomplete (create=%d getBinaryInfo=%d free=%d)",
           system.systemContextCreate != nullptr,
           system.systemContextGetBinaryInfo != nullptr,
           system.systemContextFree != nullptr);
    } else if (system.systemContextCreate(&system_context) != QNN_SUCCESS) {
      LOGE("QnnSystemContext_create failed");
      metadata_ok = false;
    }

    if (metadata_ok) {
      const Qnn_ErrorHandle_t status = system.systemContextGetBinaryInfo(
          system_context, binary_.data(), binary_.size(), &binary_info,
          &binary_info_size);
      if (status != QNN_SUCCESS) {
        LOGE("QnnSystemContext_getBinaryInfo failed: %u",
             static_cast<unsigned>(status));
        metadata_ok = false;
      }
    }
    if (metadata_ok && !get_graph_metadata(binary_info, metadata)) {
      LOGE("QNN context graph metadata unusable (see preceding reason)");
      metadata_ok = false;
    }
    if (metadata_ok &&
        !copy_tensor(metadata.inputs[0], input_, input_name_, input_dimensions_)) {
      LOGE("QNN context input tensor unusable (version %u)",
           static_cast<unsigned>(metadata.inputs[0].version));
      metadata_ok = false;
    }
    if (metadata_ok &&
        !copy_tensor(metadata.outputs[0], output_, output_name_, output_dimensions_)) {
      LOGE("QNN context output tensor unusable (version %u)",
           static_cast<unsigned>(metadata.outputs[0].version));
      metadata_ok = false;
    }
    if (!metadata_ok) {
      if (system_context && system.systemContextFree) {
        system.systemContextFree(system_context);
      }
      deactivate();
      return false;
    }
    graph_name_ = metadata.name;
    system.systemContextFree(system_context);

    if (!validate_tensor(input_) || !validate_tensor(output_) ||
        input_.v2.dimensions[1] != input_.v2.dimensions[2] ||
        output_.v2.dimensions[1] % input_.v2.dimensions[1] != 0 ||
        output_.v2.dimensions[2] % input_.v2.dimensions[2] != 0 ||
        output_.v2.dimensions[1] / input_.v2.dimensions[1] !=
            output_.v2.dimensions[2] / input_.v2.dimensions[2]) {
      LOGE("QNN context tensor layout is not square NHWC with an integer scale");
      deactivate();
      return false;
    }
    tile_size_ = static_cast<int>(input_.v2.dimensions[1]);
    output_tile_size_ = static_cast<int>(output_.v2.dimensions[1]);
    scale_ = output_tile_size_ / tile_size_;
    if (scale_ < 2 || scale_ > 4) {
      LOGE("Unsupported QNN output scale: %d", scale_);
      deactivate();
      return false;
    }

    Qnn_ErrorHandle_t status =
        qnn.contextCreateFromBinary(backend_, device_, nullptr, binary_.data(),
                                    binary_.size(), &context_, nullptr);
    if (status != QNN_SUCCESS) {
      LOGE("QNN contextCreateFromBinary failed: %u", static_cast<unsigned>(status));
      deactivate();
      return false;
    }
    status = qnn.graphRetrieve(context_, graph_name_.c_str(), &graph_);
    if (status != QNN_SUCCESS) {
      LOGE("QNN graphRetrieve failed for %s: %u", graph_name_.c_str(),
           static_cast<unsigned>(status));
      deactivate();
      return false;
    }

    const size_t input_elements = static_cast<size_t>(tile_size_) * tile_size_ * 3u;
    const size_t output_elements =
        static_cast<size_t>(output_tile_size_) * output_tile_size_ * 3u;
    if (is_fp16_tensor(input_)) {
      input_fp16_buffer_.resize(input_elements);
      set_client_buffer(input_, input_fp16_buffer_.data(),
                        input_fp16_buffer_.size() * sizeof(uint16_t));
    } else if (is_quant16_tensor(input_)) {
      input_quant16_buffer_.resize(input_elements);
      set_client_buffer(input_, input_quant16_buffer_.data(),
                        input_quant16_buffer_.size() * sizeof(uint16_t));
    } else {
      input_quant8_buffer_.resize(input_elements);
      set_client_buffer(input_, input_quant8_buffer_.data(),
                        input_quant8_buffer_.size());
    }
    if (is_fp16_tensor(output_)) {
      output_fp16_buffer_.resize(output_elements);
      set_client_buffer(output_, output_fp16_buffer_.data(),
                        output_fp16_buffer_.size() * sizeof(uint16_t));
    } else if (is_quant16_tensor(output_)) {
      output_quant16_buffer_.resize(output_elements);
      set_client_buffer(output_, output_quant16_buffer_.data(),
                        output_quant16_buffer_.size() * sizeof(uint16_t));
    } else {
      output_quant8_buffer_.resize(output_elements);
      set_client_buffer(output_, output_quant8_buffer_.data(),
                        output_quant8_buffer_.size());
    }
    pixel_tables_.build(
        [&](float value) -> uint16_t {
          if (is_fp16_tensor(input_)) return float_to_half(value);
          if (is_quant16_tensor(input_)) return quantize_uint16(value, input_);
          return quantize_uint8(value, input_);
        },
        [&](uint16_t value) -> float {
          if (is_fp16_tensor(output_)) return half_to_float(value);
          if (is_quant16_tensor(output_)) return dequantize_uint16(value, output_);
          return dequantize_uint8(static_cast<uint8_t>(value), output_);
        },
        is_quant8_tensor(output_) ? 256 : 65536);
    input_x_coordinates_.resize(tile_size_);
    use_client_buffers();
    initialize_shared_io();
    padding_ = std::clamp(padding, 0, 48);
    configure_performance();
    initialized_ = true;
    LOGD("Loaded QNN graph %s with tile %dx%d, scale %dx, %s IO and padding %d",
         graph_name_.c_str(), tile_size_, tile_size_, scale_,
         is_fp16_tensor(input_) ? "FP16" : (is_quant16_tensor(input_) ? "INT16" : "INT8"),
         padding_);
    return true;
  }

  int process(const uint8_t *input, int width, int height, int input_stride,
              uint8_t *output, int output_stride, std::atomic<int> *progress,
              const std::atomic<bool> *should_abort) {
    if (!initialized_ || !input || !output || width <= 0 || height <= 0) {
      return -1;
    }
    const int core = tile_size_ - 2 * padding_;
    if (core <= 0) {
      return -1;
    }
    const int columns = (width + core - 1) / core;
    const int rows = (height + core - 1) / core;
    const int total_tiles = columns * rows;
    QnnPowerVote power(performance_infrastructure_);
    bool power_warning_logged = false;
    int completed = 0;
    int cached_tiles = 0;
    std::chrono::nanoseconds prepare_time{0};
    std::chrono::nanoseconds execute_time{0};
    std::chrono::nanoseconds output_time{0};
    std::chrono::nanoseconds graph_time{0};
    std::chrono::nanoseconds cooling_time{0};
    const int initial_thermal = thermal_status.load();

    for (int tile_y = 0; tile_y < height; tile_y += core) {
      for (int tile_x = 0; tile_x < width; tile_x += core) {
        if (should_abort && should_abort->load()) {
          return -2;
        }
        auto stage_start = std::chrono::steady_clock::now();
        const int uniform_color = fill_input_tile(input, width, height, input_stride,
                                                  tile_x - padding_, tile_y - padding_);
        prepare_time += std::chrono::steady_clock::now() - stage_start;
        const auto &qnn = provider_->QNN_INTERFACE_VER_NAME;
        stage_start = std::chrono::steady_clock::now();
        const size_t output_bytes = output_bytes_;
        const auto *cached = uniform_tiles_.find(uniform_color, output_bytes);
        Qnn_ErrorHandle_t status = QNN_SUCCESS;
        if (cached) {
          std::memcpy(output_data_, cached, output_bytes);
          ++cached_tiles;
        } else {
          // At critical temperature release the vote while waiting. The request-scoped
          // cancellation signal still ends this wait when the reader leaves the page.
          const auto cooling_start = std::chrono::steady_clock::now();
          while (thermal_status.load() >= 4) {
            power.release();
            if (should_abort && should_abort->load()) return -2;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
          }
          cooling_time += std::chrono::steady_clock::now() - cooling_start;
          if (should_abort && should_abort->load()) return -2;
          if (!power.apply(thermal_status.load()) && !power_warning_logged) {
            LOGE("QNN performance hint unavailable; using system-managed power policy");
            power_warning_logged = true;
          }
          const auto graph_start = std::chrono::steady_clock::now();
          status = qnn.graphExecute(graph_, &input_, 1, &output_, 1, nullptr, nullptr);
          if (status != QNN_SUCCESS && shared_io_.active()) {
            LOGW("QNN shared I/O execute rejected: %u; retrying ordinary buffers", static_cast<unsigned>(status));
            const auto *shared_input = input_data_;
            use_client_buffers();
            std::memcpy(input_data_, shared_input, input_bytes_);
            shared_io_.reset();
            if (should_abort && should_abort->load()) return -2;
            status = qnn.graphExecute(graph_, &input_, 1, &output_, 1, nullptr, nullptr);
          }
          graph_time += std::chrono::steady_clock::now() - graph_start;
          if (status == QNN_SUCCESS)
            uniform_tiles_.remember(uniform_color, output_data_, output_bytes);
        }
        execute_time += std::chrono::steady_clock::now() - stage_start;
        if (status != QNN_SUCCESS) {
          LOGE("QNN graphExecute failed: %u", static_cast<unsigned>(status));
          return -1;
        }

        const int copy_width = std::min(core, width - tile_x) * scale_;
        const int copy_height = std::min(core, height - tile_y) * scale_;
        stage_start = std::chrono::steady_clock::now();
        write_output_tile(output, output_stride, tile_x * scale_,
                          tile_y * scale_,
                          copy_width, copy_height, input, width, height,
                          input_stride);
        output_time += std::chrono::steady_clock::now() - stage_start;
        ++completed;
        if (progress) {
          progress->store(std::min(99, completed * 100 / total_tiles));
        }
      }
    }
    if (progress) {
      progress->store(100);
    }
    LOGD("QNN tile profile: count=%d cached=%d tile=%d core=%d prepare=%lldms "
         "execute=%lldms output=%lldms thermal=%d io=%s input=%dx%d output=%dx%d "
         "graph=%lldms cooling=%lldms initialThermal=%d",
         total_tiles, cached_tiles, tile_size_, core,
         static_cast<long long>(
             std::chrono::duration_cast<std::chrono::milliseconds>(prepare_time)
                 .count()),
         static_cast<long long>(
             std::chrono::duration_cast<std::chrono::milliseconds>(execute_time)
                 .count()),
         static_cast<long long>(
             std::chrono::duration_cast<std::chrono::milliseconds>(output_time)
                 .count()), thermal_status.load(), shared_io_.active() ? "shared" : "ordinary",
         width, height, width * scale_, height * scale_,
         static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(graph_time).count()),
         static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(cooling_time).count()),
         initial_thermal);
    return 0;
  }

  bool initialized() const { return initialized_; }
  int scale() const { return scale_; }

  void deactivate() {
    initialized_ = false;
    shared_io_.reset();
    input_data_ = output_data_ = nullptr;
    input_bytes_ = output_bytes_ = 0;
    if (provider_) {
      const auto &qnn = provider_->QNN_INTERFACE_VER_NAME;
      if (context_ && qnn.contextFree) {
        qnn.contextFree(context_, nullptr);
      }
    }
    release_performance();
    graph_ = nullptr;
    context_ = nullptr;
    input_fp16_buffer_.clear();
    output_fp16_buffer_.clear();
    input_quant16_buffer_.clear();
    output_quant16_buffer_.clear();
    input_quant8_buffer_.clear();
    output_quant8_buffer_.clear();
    binary_.clear();
    uniform_tiles_.clear();
    tile_size_ = 0;
    output_tile_size_ = 0;
    scale_ = 0;
  }

  void reset() {
    deactivate();
    if (provider_) {
      const auto &qnn = provider_->QNN_INTERFACE_VER_NAME;
      if (device_ && qnn.deviceFree) {
        qnn.deviceFree(device_);
      }
      if (backend_ && qnn.backendFree) {
        qnn.backendFree(backend_);
      }
      // The logger must outlive the backend that holds it.
      if (log_ && qnn.logFree) {
        qnn.logFree(log_);
      }
    }
    log_ = nullptr;
    device_ = nullptr;
    backend_ = nullptr;
    provider_ = nullptr;
    system_provider_ = nullptr;
    if (rpc_library_) {
      dlclose(rpc_library_);
      rpc_library_ = nullptr;
    }
    if (system_library_) {
      dlclose(system_library_);
      system_library_ = nullptr;
    }
    // ModelDlc is loaded BEFORE libQnnHtp.so, so it must be released AFTER it
    // (reverse order) — unloading a library whose dependents are still mapped
    // would leave the HTP backend with dangling entry points.
    if (model_dlc_library_) {
      dlclose(model_dlc_library_);
      model_dlc_library_ = nullptr;
    }
    if (backend_library_) {
      dlclose(backend_library_);
      backend_library_ = nullptr;
    }
  }

private:
  void use_client_buffers() {
    input_data_ = is_fp16_tensor(input_) ? static_cast<void *>(input_fp16_buffer_.data())
        : is_quant16_tensor(input_) ? static_cast<void *>(input_quant16_buffer_.data())
        : static_cast<void *>(input_quant8_buffer_.data());
    output_data_ = is_fp16_tensor(output_) ? static_cast<void *>(output_fp16_buffer_.data())
        : is_quant16_tensor(output_) ? static_cast<void *>(output_quant16_buffer_.data())
        : static_cast<void *>(output_quant8_buffer_.data());
    const size_t input_elements = static_cast<size_t>(tile_size_) * tile_size_ * 3;
    const size_t output_elements = static_cast<size_t>(output_tile_size_) * output_tile_size_ * 3;
    input_bytes_ = input_elements * (is_quant8_tensor(input_) ? 1 : 2);
    output_bytes_ = output_elements * (is_quant8_tensor(output_) ? 1 : 2);
    set_client_buffer(input_, input_data_, static_cast<uint32_t>(input_bytes_));
    set_client_buffer(output_, output_data_, static_cast<uint32_t>(output_bytes_));
  }

  void initialize_shared_io() {
    if (!rpc_library_) rpc_library_ = dlopen("libcdsprpc.so", RTLD_NOW | RTLD_LOCAL);
    if (!rpc_library_) { LOGD("QNN RPC allocator unavailable; using ordinary I/O"); return; }
    RpcMemoryApi api;
    api.allocate = reinterpret_cast<decltype(api.allocate)>(dlsym(rpc_library_, "rpcmem_alloc"));
    api.release = reinterpret_cast<decltype(api.release)>(dlsym(rpc_library_, "rpcmem_free"));
    api.to_fd = reinterpret_cast<decltype(api.to_fd)>(dlsym(rpc_library_, "rpcmem_to_fd"));
    const auto &qnn = provider_->QNN_INTERFACE_VER_NAME;
    if (shared_io_.initialize(context_, qnn.memRegister, qnn.memDeRegister, api, input_, output_)) {
      input_data_ = shared_io_.input_data();
      output_data_ = shared_io_.output_data();
      shared_io_.bind(input_, output_);
      LOGD("QNN model-lifetime registered shared I/O enabled: input=%zu output=%zu bytes", input_bytes_, output_bytes_);
    } else {
      LOGD("QNN shared I/O registration unavailable; using ordinary I/O");
    }
  }

  bool ensure_runtime() {
    if (backend_ && provider_ && system_provider_) {
      return true;
    }
    ensure_dsp_path(nullptr); // 兜底：任何时候走到这里都保证 env 已就位
    reset();
    // Load order matters: libQnnModelDlc.so first, so that when libQnnHtp.so is
    // mapped it can already resolve the DLC entry points it dlopen()s by name.
    // release order in reset() is the exact reverse.
    model_dlc_library_ = dlopen("libQnnModelDlc.so", RTLD_NOW | RTLD_LOCAL);
    if (!model_dlc_library_) {
      // Non-fatal on purpose: some QNN runtime builds fold these entry points
      // into libQnnHtp.so itself, so a missing ModelDlc must not abort init
      // here — the DLC call reports its own error if it really is needed.
      LOGW("libQnnModelDlc.so not available: %s", dlerror());
    }
    backend_library_ = dlopen("libQnnHtp.so", RTLD_NOW | RTLD_LOCAL);
    system_library_ = dlopen("libQnnSystem.so", RTLD_NOW | RTLD_LOCAL);
    // Why ModelDlc is loaded at all: it carries the DLC container reader and the
    // QnnModel_composeGraphs / composeGraphsFromDlc entry points. It is NOT in
    // the DT_NEEDED list of libQnnHtp.so and there is no public header to link
    // against; the reference implementation (Mihon's mihon_img_upscale,
    // package app.mihon) dlopen()s it by name exactly like this.
    //
    // NOTE (2026-09-18, corrected twice — do not trust the earlier versions of
    // this comment): neither ModelDlc nor libQnnHtpPrepare.so is the cause of
    // `loadRemoteSymbols failed ... 4000` / `Failed to load skel, error: 4000`.
    // Both were added to jniLibs and both left the on-device error chain
    // byte-for-byte unchanged (verified with logcat diff before/after).
    // The failure is in the DSP transport / skel-load stage, and the only
    // remaining difference against the known-good reference build (app.mihon
    // 1.3.9, same device, all V75 files byte-identical) was that the reference
    // ships libQnnHtpV{69,73,81}Skel/Stub.so as well — those are packaged now.
    // (2026-09-19: the QNN runtime was upgraded from 2.49.0 to qnn-runtime 2.50.0 so
    // that "runtime version >= context compile version" holds for the fp16 contexts
    // compiled locally by onnxruntime-qnn (they carry QAIRT 2.49.40). NOTE: the
    // upgrade was originally believed to *fix* those contexts — it did not. Their real
    // problem was a version-1 graph tensor plus an NCHW I/O layout; see copy_tensor()
    // below and the metadata block in load(). The upgrade is kept because the version
    // relation is worth having, not because it was the cure. These .so files are
    // therefore no longer byte-identical to the reference build's, by design.)
    // ModelDlc is kept because the DLC graph path needs it, not because it was
    // ever the culprit.
    if (!backend_library_ || !system_library_) {
      LOGE("Unable to load QNN libraries: %s", dlerror());
      reset();
      return false;
    }
    provider_ = find_qnn_provider(backend_library_);
    system_provider_ = find_system_provider(system_library_);
    if (!provider_ || !system_provider_) {
      LOGE("Compatible QNN providers were not found");
      reset();
      return false;
    }

    const auto &qnn = provider_->QNN_INTERFACE_VER_NAME;
    if (!qnn.backendCreate || !qnn.contextCreateFromBinary ||
        !qnn.graphRetrieve || !qnn.graphExecute) {
      LOGE("QNN HTP provider is missing required APIs");
      reset();
      return false;
    }
    // Komiho (2026-09-18): see qnn_log_callback above — verbose logging for the
    // init path only, so the next device log names the skel that fails to load.
    // Best effort: on any failure we simply fall back to QNN's default logger.
    if (qnn.logCreate && !log_) {
      if (qnn.logCreate(&qnn_log_callback, QNN_LOG_LEVEL_DEBUG, &log_) !=
          QNN_SUCCESS) {
        log_ = nullptr;
      }
    }
    Qnn_ErrorHandle_t status = qnn.backendCreate(log_, nullptr, &backend_);
    if (status != QNN_SUCCESS) {
      LOGE("QNN backendCreate failed: %u", static_cast<unsigned>(status));
      reset();
      return false;
    }
    if (qnn.deviceCreate) {
      status = qnn.deviceCreate(nullptr, nullptr, &device_);
      if (status == QNN_DEVICE_ERROR_UNSUPPORTED_FEATURE ||
          status == QNN_DEVICE_ERROR_INVALID_CONFIG) {
        LOGD("QNN deviceCreate returned %u; using the backend default device",
             static_cast<unsigned>(status));
        device_ = nullptr;
      } else if (status != QNN_SUCCESS) {
        LOGE("QNN deviceCreate failed: %u", static_cast<unsigned>(status));
        reset();
        return false;
      }
    }
    // Back to quiet for the inference path (verbose logging costs time).
    if (log_ && qnn.logSetLogLevel) {
      qnn.logSetLogLevel(log_, QNN_LOG_LEVEL_ERROR);
    }
    return true;
  }

  void configure_performance() {
    const auto &qnn = provider_->QNN_INTERFACE_VER_NAME;
    if (!qnn.deviceGetInfrastructure) {
      return;
    }
    QnnDevice_Infrastructure_t device_infrastructure = nullptr;
    Qnn_ErrorHandle_t status =
        qnn.deviceGetInfrastructure(&device_infrastructure);
    if (status != QNN_SUCCESS || !device_infrastructure) {
      LOGE("QNN deviceGetInfrastructure failed: %u",
           static_cast<unsigned>(status));
      return;
    }
    auto *htp_infrastructure = reinterpret_cast<QnnHtpDevice_Infrastructure_t *>(
        device_infrastructure);
    if (htp_infrastructure->infraType !=
        QNN_HTP_DEVICE_INFRASTRUCTURE_TYPE_PERF) {
      return;
    }
    performance_infrastructure_ = htp_infrastructure->perfInfra;
    if (!performance_infrastructure_.createPowerConfigId ||
        !performance_infrastructure_.setPowerConfig ||
        !performance_infrastructure_.destroyPowerConfigId) {
      return;
    }
    // Only discover the interface here; no persistent frequency or no-sleep vote.
    LOGD("QNN HTP scoped thermal-aware performance available");
  }

  void release_performance() {
    performance_infrastructure_ = QNN_HTP_DEVICE_PERF_INFRASTRUCTURE_INIT;
  }

  static const QnnInterface_t *find_qnn_provider(void *library) {
    auto get_providers = reinterpret_cast<GetProvidersFn>(
        dlsym(library, "QnnInterface_getProviders"));
    if (!get_providers) {
      return nullptr;
    }
    const QnnInterface_t **providers = nullptr;
    uint32_t count = 0;
    if (get_providers(&providers, &count) != QNN_SUCCESS || !providers) {
      return nullptr;
    }
    for (uint32_t i = 0; i < count; ++i) {
      const auto &version = providers[i]->apiVersion.coreApiVersion;
      if (version.major == QNN_API_VERSION_MAJOR &&
          version.minor >= QNN_API_VERSION_MINOR) {
        return providers[i];
      }
    }
    return nullptr;
  }

  static const QnnSystemInterface_t *find_system_provider(void *library) {
    auto get_providers = reinterpret_cast<GetSystemProvidersFn>(
        dlsym(library, "QnnSystemInterface_getProviders"));
    if (!get_providers) {
      return nullptr;
    }
    const QnnSystemInterface_t **providers = nullptr;
    uint32_t count = 0;
    if (get_providers(&providers, &count) != QNN_SUCCESS || !providers) {
      return nullptr;
    }
    for (uint32_t i = 0; i < count; ++i) {
      const auto &version = providers[i]->systemApiVersion;
      if (version.major == QNN_SYSTEM_API_VERSION_MAJOR &&
          version.minor >= QNN_SYSTEM_API_VERSION_MINOR) {
        return providers[i];
      }
    }
    return nullptr;
  }

  // Komiho (2026-09-19): accept BOTH tensor versions, and always emit a version-2 tensor.
  //
  // Contexts produced by `onnxruntime-qnn` (our local compile path) serialise their graph
  // tensors as **QNN_TENSOR_VERSION_1**, whereas the QAIRT `qnn-context-binary-generator`
  // output upstream ships is version 2. Rejecting v1 made every locally compiled model fall
  // back to Vulkan on device — and because this function sits in the same condition as
  // `systemContextGetBinaryInfo` and `get_graph_metadata`, the only symptom was the generic
  // "Unable to read QNN context graph metadata" line (see SKILL.md for that round).
  //
  // Qnn_TensorV1_t and Qnn_TensorV2_t share the layout of every field we consume
  // (id / name / type / dataFormat / dataType / quantizeParams / rank / dimensions); V2 only
  // appends isDynamicDimensions / sparseParams / isProduced. So read through the matching
  // member and write a freshly zeroed V2 — which is what graphExecute and validate_tensor()
  // below expect.
  static bool copy_tensor(const Qnn_Tensor_t &source, Qnn_Tensor_t &destination,
                          std::string &name, std::vector<uint32_t> &dimensions) {
    const char *source_name = nullptr;
    const uint32_t *source_dimensions = nullptr;
    uint32_t source_rank = 0;
    switch (source.version) {
    case QNN_TENSOR_VERSION_2:
      source_name = source.v2.name;
      source_dimensions = source.v2.dimensions;
      source_rank = source.v2.rank;
      break;
    case QNN_TENSOR_VERSION_1:
      source_name = source.v1.name;
      source_dimensions = source.v1.dimensions;
      source_rank = source.v1.rank;
      break;
    default:
      return false;
    }
    if (!source_name || !source_dimensions || source_rank == 0) {
      return false;
    }
    name = source_name;
    dimensions.assign(source_dimensions, source_dimensions + source_rank);

    std::memset(&destination, 0, sizeof(destination));
    destination.version = QNN_TENSOR_VERSION_2;
    if (source.version == QNN_TENSOR_VERSION_2) {
      destination.v2.id = source.v2.id;
      destination.v2.type = source.v2.type;
      destination.v2.dataFormat = source.v2.dataFormat;
      destination.v2.dataType = source.v2.dataType;
      destination.v2.quantizeParams = source.v2.quantizeParams;
      destination.v2.sparseParams = source.v2.sparseParams;
    } else {
      destination.v2.id = source.v1.id;
      destination.v2.type = source.v1.type;
      destination.v2.dataFormat = source.v1.dataFormat;
      destination.v2.dataType = source.v1.dataType;
      destination.v2.quantizeParams = source.v1.quantizeParams;
      // V1 has no sparseParams field, and a plain memset would leave type == 0, which is
      // QNN_SPARSE_LAYOUT_HYBRID_COO — i.e. "this tensor is sparse". These graphs are dense,
      // so say so explicitly.
      destination.v2.sparseParams.type = QNN_SPARSE_LAYOUT_UNDEFINED;
    }
    destination.v2.name = name.c_str();
    destination.v2.rank = source_rank;
    destination.v2.dimensions = dimensions.data();
    destination.v2.memType = QNN_TENSORMEMTYPE_RAW;
    destination.v2.isDynamicDimensions = nullptr;
    return true;
  }

  static bool is_fp16_tensor(const Qnn_Tensor_t &tensor) {
    return tensor.v2.dataType == QNN_DATATYPE_FLOAT_16;
  }

  static bool is_quant8_tensor(const Qnn_Tensor_t &tensor) {
    return tensor.v2.dataType == QNN_DATATYPE_UFIXED_POINT_8 &&
           tensor.v2.quantizeParams.encodingDefinition == QNN_DEFINITION_DEFINED &&
           tensor.v2.quantizeParams.quantizationEncoding ==
               QNN_QUANTIZATION_ENCODING_SCALE_OFFSET &&
           tensor.v2.quantizeParams.scaleOffsetEncoding.scale > 0.0f;
  }

  static bool is_quant16_tensor(const Qnn_Tensor_t &tensor) {
    return tensor.v2.dataType == QNN_DATATYPE_UFIXED_POINT_16 &&
           tensor.v2.quantizeParams.encodingDefinition == QNN_DEFINITION_DEFINED &&
           tensor.v2.quantizeParams.quantizationEncoding ==
               QNN_QUANTIZATION_ENCODING_SCALE_OFFSET &&
           tensor.v2.quantizeParams.scaleOffsetEncoding.scale > 0.0f;
  }

  static bool validate_tensor(const Qnn_Tensor_t &tensor) {
    return tensor.version == QNN_TENSOR_VERSION_2 && tensor.v2.rank == 4 &&
           (is_fp16_tensor(tensor) || is_quant8_tensor(tensor) ||
            is_quant16_tensor(tensor)) &&
           tensor.v2.dimensions[0] == 1 && tensor.v2.dimensions[1] > 0 &&
           tensor.v2.dimensions[2] > 0 && tensor.v2.dimensions[3] == 3;
  }

  static uint8_t quantize_uint8(float value, const Qnn_Tensor_t &tensor) {
    const auto &encoding = tensor.v2.quantizeParams.scaleOffsetEncoding;
    const float quantized = value / encoding.scale - encoding.offset;
    return static_cast<uint8_t>(
        std::clamp(static_cast<int>(std::lround(quantized)), 0, 255));
  }

  static float dequantize_uint8(uint8_t value, const Qnn_Tensor_t &tensor) {
    const auto &encoding = tensor.v2.quantizeParams.scaleOffsetEncoding;
    return (static_cast<int>(value) + encoding.offset) * encoding.scale;
  }

  static uint16_t quantize_uint16(float value, const Qnn_Tensor_t &tensor) {
    const auto &encoding = tensor.v2.quantizeParams.scaleOffsetEncoding;
    const long quantized = std::lround(value / encoding.scale - encoding.offset);
    return static_cast<uint16_t>(std::clamp(quantized, 0L, 65535L));
  }

  static float dequantize_uint16(uint16_t value, const Qnn_Tensor_t &tensor) {
    const auto &encoding = tensor.v2.quantizeParams.scaleOffsetEncoding;
    return (static_cast<int32_t>(value) + encoding.offset) * encoding.scale;
  }

  static void set_client_buffer(Qnn_Tensor_t &tensor, void *data, uint32_t size) {
    tensor.v2.memType = QNN_TENSORMEMTYPE_RAW;
    tensor.v2.clientBuf.data = data;
    tensor.v2.clientBuf.dataSize = size;
  }

  int fill_input_tile(const uint8_t *input, int width, int height, int stride,
                       int origin_x, int origin_y) {
    const auto pack = [&](auto *destination) {
      return upscale_pixels::pack_tile(input, width, height, stride, origin_x, origin_y,
                                tile_size_, pixel_tables_.input.data(), destination,
                                input_x_coordinates_.data());
    };
    if (is_fp16_tensor(input_) || is_quant16_tensor(input_)) return pack(static_cast<uint16_t *>(input_data_));
    return pack(static_cast<uint8_t *>(input_data_));
  }

  void write_output_tile(uint8_t *output, int output_stride, int target_x,
                         int target_y, int copy_width, int copy_height,
                         const uint8_t *input, int width, int height,
                         int input_stride) const {
    const auto unpack = [&](const auto *source) {
      upscale_pixels::unpack_tile(source, output_tile_size_, padding_ * scale_,
                                  pixel_tables_.output.data(), output, output_stride,
                                  target_x, target_y, copy_width, copy_height,
                                  input, input_stride, scale_);
    };
    if (is_fp16_tensor(output_) || is_quant16_tensor(output_)) unpack(static_cast<const uint16_t *>(output_data_));
    else unpack(static_cast<const uint8_t *>(output_data_));
  }

  void *backend_library_ = nullptr;
  void *system_library_ = nullptr;
  // Held only to keep the DLC entry points resolvable for libQnnHtp.so; we
  // never call into it directly (there is no public header for it).
  void *model_dlc_library_ = nullptr;
  const QnnInterface_t *provider_ = nullptr;
  const QnnSystemInterface_t *system_provider_ = nullptr;
  // Komiho (2026-09-18): verbose logger for the init path only (see qnn_log_callback).
  Qnn_LogHandle_t log_ = nullptr;
  Qnn_BackendHandle_t backend_ = nullptr;
  Qnn_DeviceHandle_t device_ = nullptr;
  Qnn_ContextHandle_t context_ = nullptr;
  Qnn_GraphHandle_t graph_ = nullptr;
  Qnn_Tensor_t input_ = QNN_TENSOR_INIT;
  Qnn_Tensor_t output_ = QNN_TENSOR_INIT;
  std::string graph_name_;
  std::string input_name_;
  std::string output_name_;
  std::vector<uint32_t> input_dimensions_;
  std::vector<uint32_t> output_dimensions_;
  std::vector<uint8_t> binary_;
  std::vector<uint16_t> input_fp16_buffer_;
  std::vector<uint16_t> output_fp16_buffer_;
  std::vector<uint16_t> input_quant16_buffer_;
  std::vector<uint16_t> output_quant16_buffer_;
  std::vector<uint8_t> input_quant8_buffer_;
  std::vector<uint8_t> output_quant8_buffer_;
  upscale_pixels::ConversionTables pixel_tables_;
  std::vector<int> input_x_coordinates_;
  upscale_pixels::UniformTileCache uniform_tiles_;
  QnnSharedIo shared_io_;
  void *rpc_library_ = nullptr;
  void *input_data_ = nullptr;
  void *output_data_ = nullptr;
  size_t input_bytes_ = 0;
  size_t output_bytes_ = 0;
  int padding_ = 0;
  int tile_size_ = 0;
  int output_tile_size_ = 0;
  int scale_ = 0;
  QnnHtpDevice_PerfInfrastructure_t performance_infrastructure_ =
      QNN_HTP_DEVICE_PERF_INFRASTRUCTURE_INIT;
  bool initialized_ = false;
};

Runtime runtime;

} // namespace
#endif

bool is_runtime_loadable() {
#if MIHON_ENABLE_QNN
  return runtime.probe();
#else
  return false;
#endif
}

int architecture() {
#if MIHON_ENABLE_QNN
  return runtime.architecture();
#else
  return 0;
#endif
}

bool initialize(const std::string &context_path, int padding) {
#if MIHON_ENABLE_QNN
  return runtime.load(context_path, padding);
#else
  (void)context_path;
  (void)padding;
  return false;
#endif
}

bool is_initialized() {
#if MIHON_ENABLE_QNN
  return runtime.initialized();
#else
  return false;
#endif
}

int scale() {
#if MIHON_ENABLE_QNN
  return runtime.scale();
#else
  return 0;
#endif
}

int process_rgba(const uint8_t *input, int width, int height, int input_stride,
                 uint8_t *output, int output_stride,
                 std::atomic<int> *progress,
                 const std::atomic<bool> *should_abort) {
#if MIHON_ENABLE_QNN
  return runtime.process(input, width, height, input_stride, output,
                         output_stride, progress, should_abort);
#else
  (void)input;
  (void)width;
  (void)height;
  (void)input_stride;
  (void)output;
  (void)output_stride;
  (void)progress;
  (void)should_abort;
  return -1;
#endif
}

void shutdown() {
#if MIHON_ENABLE_QNN
  runtime.deactivate();
#endif
}

} // namespace qnn_backend
