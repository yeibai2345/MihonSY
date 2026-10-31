#include "qnn_shared_io.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

static int allocations, releases, registrations, deregistrations;
static int fail_allocation;
static bool fail_fd, fail_registration, context_alive;
static std::vector<void *> live_buffers;
static void *allocate(int heap, uint32_t flags, int size) {
  assert(heap == 25 && flags == 1 && (size == 192 || size == 768));
  if (++allocations == fail_allocation) return nullptr;
  auto *data = std::malloc(size);
  live_buffers.push_back(data);
  return data;
}
static void release(void *data) {
  assert(deregistrations > 0 || registrations == 0 || fail_registration);
  ++releases;
  for (auto &buffer : live_buffers) if (buffer == data) buffer = nullptr;
  std::free(data);
}
static int to_fd(void *) { return fail_fd ? -1 : 73; }
static Qnn_ErrorHandle_t register_memory(Qnn_ContextHandle_t, const Qnn_MemDescriptor_t *desc,
                                        uint32_t count, Qnn_MemHandle_t *handles) {
  assert(context_alive && count == 2);
  ++registrations;
  for (uint32_t i = 0; i < count; ++i) {
    assert(desc[i].memType == QNN_MEM_TYPE_ION && desc[i].ionInfo.fd == 73);
    assert(desc[i].dataType == QNN_DATATYPE_UFIXED_POINT_8);
    assert(desc[i].memShape.numDim == 4 && desc[i].memShape.dimSize[0] == 1);
    assert(desc[i].memShape.dimSize[1] == (i == 0 ? 8 : 16));
  }
  if (fail_registration) return 1;
  handles[0] = reinterpret_cast<Qnn_MemHandle_t>(11);
  handles[1] = reinterpret_cast<Qnn_MemHandle_t>(12);
  return QNN_SUCCESS;
}
static Qnn_ErrorHandle_t deregister_memory(const Qnn_MemHandle_t *handles, uint32_t count) {
  assert(context_alive && count == 2 && handles[0] && handles[1]);
  assert(releases == 0); // Never free storage while QNN can still access it.
  ++deregistrations;
  return QNN_SUCCESS;
}
static Qnn_Tensor_t tensor(uint32_t *dims, uint32_t bytes) {
  auto result = Qnn_Tensor_t QNN_TENSOR_INIT;
  result.version = QNN_TENSOR_VERSION_2;
  result.v2.rank = 4;
  result.v2.dimensions = dims;
  result.v2.dataType = QNN_DATATYPE_UFIXED_POINT_8;
  result.v2.memType = QNN_TENSORMEMTYPE_RAW;
  result.v2.clientBuf.dataSize = bytes;
  return result;
}
static void reset() {
  for (auto buffer : live_buffers) assert(!buffer);
  live_buffers.clear();
  allocations = releases = registrations = deregistrations = fail_allocation = 0;
  fail_fd = fail_registration = false;
  context_alive = true;
}
int main() {
  uint32_t in_dims[] = {1, 8, 8, 3}, out_dims[] = {1, 16, 16, 3};
  const auto context = reinterpret_cast<Qnn_ContextHandle_t>(1);
  RpcMemoryApi api{allocate, release, to_fd};
  reset();
  {
    QnnSharedIo io;
    auto input = tensor(in_dims, 192), output = tensor(out_dims, 768);
    assert(io.initialize(context, register_memory, deregister_memory, api, input, output));
    assert(io.active() && allocations == 2 && registrations == 1);
    // The producer and consumer use the exact registered storage, without a
    // quantization or precision conversion at the memory registration boundary.
    std::memset(io.input_data(), 37, 192);
    assert(static_cast<unsigned char *>(live_buffers[0])[191] == 37);
    io.bind(input, output);
    assert(input.v2.memType == QNN_TENSORMEMTYPE_MEMHANDLE);
    assert(input.v2.memHandle == reinterpret_cast<Qnn_MemHandle_t>(11));
    assert(output.v2.memHandle == reinterpret_cast<Qnn_MemHandle_t>(12));
    io.reset();
    assert(!io.active() && deregistrations == 1 && releases == 2);
    io.reset();
    assert(deregistrations == 1 && releases == 2);
  }
  context_alive = false;
  for (int failure : {1, 2, 3, 4}) {
    reset();
    fail_allocation = failure <= 2 ? failure : 0;
    fail_fd = failure == 3;
    fail_registration = failure == 4;
    {
      QnnSharedIo io;
      const auto input = tensor(in_dims, 192), output = tensor(out_dims, 768);
      assert(!io.initialize(context, register_memory, deregister_memory, api, input, output));
      assert(!io.active() && deregistrations == 0);
      assert(input.v2.memType == QNN_TENSORMEMTYPE_RAW); // Ordinary fallback remains intact.
    }
    for (auto buffer : live_buffers) assert(!buffer);
  }
  reset();
  {
    QnnSharedIo io;
    const auto input = tensor(in_dims, 192), output = tensor(out_dims, 768);
    assert(!io.initialize(context, nullptr, deregister_memory, api, input, output));
    assert(allocations == 0);
    assert(io.initialize(context, register_memory, deregister_memory, api, input, output));
    // Automatic cleanup on an early caller return must precede context teardown.
  }
  assert(deregistrations == 1 && releases == 2);
  std::cout << "QNN shared I/O ownership, registration, failure fallback and cleanup checks passed.\n";
}
