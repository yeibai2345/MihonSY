#pragma once
#include <QnnInterface.h>
#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>

struct RpcMemoryApi {
  void *(*allocate)(int, uint32_t, int) = nullptr;
  void (*release)(void *) = nullptr;
  int (*to_fd)(void *) = nullptr;
};

// Model-lifetime registered buffers avoid staging each tile through raw QNN I/O.
// The caller retains ordinary buffers for devices that reject shared memory.
class QnnSharedIo {
 public:
  ~QnnSharedIo() { reset(); }
  QnnSharedIo() = default;
  QnnSharedIo(const QnnSharedIo &) = delete;
  QnnSharedIo &operator=(const QnnSharedIo &) = delete;

  bool initialize(Qnn_ContextHandle_t context, QnnMem_RegisterFn_t register_memory,
                  QnnMem_DeRegisterFn_t deregister_memory, RpcMemoryApi api,
                  const Qnn_Tensor_t &input, const Qnn_Tensor_t &output) {
    reset();
    if (!context || !register_memory || !deregister_memory ||
        !api.allocate || !api.release || !api.to_fd) return false;
    const Qnn_Tensor_t *tensors[] = {&input, &output};
    for (const auto *tensor : tensors) {
      if (tensor->version != QNN_TENSOR_VERSION_2 ||
          tensor->v2.memType != QNN_TENSORMEMTYPE_RAW ||
          tensor->v2.clientBuf.dataSize == 0 || tensor->v2.clientBuf.dataSize > INT_MAX) return false;
    }
    api_ = api;
    deregister_ = deregister_memory;
    Qnn_MemDescriptor_t descriptors[] = {QNN_MEM_DESCRIPTOR_INIT, QNN_MEM_DESCRIPTOR_INIT};
    for (size_t i = 0; i < buffers_.size(); ++i) {
      constexpr int system_heap = 25;
      constexpr uint32_t default_flags = 1;
      buffers_[i] = api_.allocate(system_heap, default_flags, static_cast<int>(tensors[i]->v2.clientBuf.dataSize));
      if (!buffers_[i]) { reset(); return false; }
      const int fd = api_.to_fd(buffers_[i]);
      if (fd < 0) { reset(); return false; }
      descriptors[i].memShape.numDim = tensors[i]->v2.rank;
      descriptors[i].memShape.dimSize = tensors[i]->v2.dimensions;
      descriptors[i].dataType = tensors[i]->v2.dataType;
      descriptors[i].memType = QNN_MEM_TYPE_ION;
      descriptors[i].ionInfo.fd = fd;
    }
    if (register_memory(context, descriptors, 2, handles_.data()) != QNN_SUCCESS ||
        !handles_[0] || !handles_[1]) { reset(); return false; }
    return true;
  }

  bool active() const { return handles_[0] && handles_[1]; }
  void *input_data() const { return buffers_[0]; }
  void *output_data() const { return buffers_[1]; }

  void bind(Qnn_Tensor_t &input, Qnn_Tensor_t &output) const {
    input.v2.memType = output.v2.memType = QNN_TENSORMEMTYPE_MEMHANDLE;
    input.v2.memHandle = handles_[0];
    output.v2.memHandle = handles_[1];
  }

  void reset() {
    // Deregister before freeing RPC storage and before the owning context is freed.
    std::array<Qnn_MemHandle_t, 2> valid{};
    uint32_t count = 0;
    for (auto handle : handles_) if (handle) valid[count++] = handle;
    if (count && deregister_) deregister_(valid.data(), count);
    handles_ = {};
    for (auto &buffer : buffers_) {
      if (buffer && api_.release) api_.release(buffer);
      buffer = nullptr;
    }
    api_ = {};
    deregister_ = nullptr;
  }

 private:
  RpcMemoryApi api_{};
  QnnMem_DeRegisterFn_t deregister_ = nullptr;
  std::array<void *, 2> buffers_{};
  std::array<Qnn_MemHandle_t, 2> handles_{};
};
