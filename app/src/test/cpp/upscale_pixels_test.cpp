#include "upscale_pixels.h"
#include <cassert>
#include <chrono>
#include <iostream>
#include <limits>
#include <random>
#include <vector>

using namespace upscale_pixels;

static void conversion_and_edges() {
  ConversionTables tables;
  const float input_scale = 0.00003125f;
  const int input_offset = -123;
  const float output_scale = 0.00004f;
  const int output_offset = -500;
  tables.build(
      [&](float v) { return static_cast<uint16_t>(std::clamp(std::lround(v / input_scale - input_offset), 0L, 65535L)); },
      [&](uint16_t v) { return (static_cast<int>(v) + output_offset) * output_scale; }, 65536);
  for (int i = 0; i < 256; ++i)
    assert(tables.input[i] == std::clamp(std::lround((i / 255.0f) / input_scale - input_offset), 0L, 65535L));
  for (int i = 0; i < 65536; ++i) {
    const float value = std::clamp((i + output_offset) * output_scale, 0.0f, 1.0f);
    assert(tables.output[i] == static_cast<uint8_t>(value * 255.0f + 0.5f));
  }
  assert(output_byte(std::numeric_limits<float>::quiet_NaN()) == 0);
  assert(output_byte(std::numeric_limits<float>::infinity()) == 255);

  std::mt19937 rng(131);
  for (int width : {1, 3, 19}) {
    for (int height : {1, 2, 27}) {
      const int stride = width * 4 + 16;
      std::vector<uint8_t> input(stride * height);
      for (auto &value : input) value = static_cast<uint8_t>(rng());
      const int side = 12;
      std::vector<uint16_t> packed(side * side * 3);
      std::vector<int> coordinates(side);
      for (int origin_x : {-35, 0, 15}) {
        for (int origin_y : {-35, 0, 22}) {
          pack_tile(input.data(), width, height, stride, origin_x, origin_y,
                     side, tables.input.data(), packed.data(), coordinates.data());
          auto reference_reflect = [](int x, int length) {
            if (length == 1) return 0;
            while (x < 0 || x >= length) x = x < 0 ? -x : 2 * length - 2 - x;
            return x;
          };
          for (int y = 0; y < side; ++y) for (int x = 0; x < side; ++x) for (int c = 0; c < 3; ++c) {
            const uint8_t value = input[reference_reflect(origin_y + y, height) * stride +
                                       reference_reflect(origin_x + x, width) * 4 + c];
            assert(packed[(y * side + x) * 3 + c] == tables.input[value]);
          }
        }
      }
    }
  }
}

static void output_geometry_and_alpha() {
  ConversionTables tables;
  tables.build([](float v) { return static_cast<uint16_t>(v * 255); },
               [](uint16_t v) { return v / 65535.0f; }, 65536);
  std::mt19937 rng(2);
  const int width = 7, height = 13, stride = width * 4 + 8;
  std::vector<uint8_t> input(stride * height);
  for (auto &value : input) value = static_cast<uint8_t>(rng());
  for (int scale : {2, 3, 4, 6}) {
    const int side = 8 * scale;
    std::vector<uint16_t> tensor(side * side * 3);
    for (auto &value : tensor) value = static_cast<uint16_t>(rng());
    const int out_stride = width * scale * 4 + 16;
    std::vector<uint8_t> output(out_stride * height * scale, 17);
    const int tx = 2 * scale, ty = 4 * scale, cw = 3 * scale, ch = 5 * scale, halo = scale;
    unpack_tile(tensor.data(), side, halo, tables.output.data(), output.data(),
                 out_stride, tx, ty, cw, ch, input.data(), stride, scale);
    for (int y = 0; y < height * scale; ++y) for (int x = 0; x < out_stride; ++x) {
      const bool written = y >= ty && y < ty + ch && x >= tx * 4 && x < (tx + cw) * 4;
      const uint8_t value = output[y * out_stride + x];
      if (!written) { assert(value == 17); continue; }
      const int column = x / 4, channel = x % 4;
      if (channel == 3) assert(value == input[(y / scale) * stride + (column / scale) * 4 + 3]);
      else assert(value == tables.output[tensor[((y - ty + halo) * side + column - tx + halo) * 3 + channel]]);
    }
  }
}

static void gray_and_cache() {
  uint8_t gray[] = {90, 94, 98, 77};
  uint8_t color[] = {90, 94, 99, 77};
  assert(near_gray(gray));
  assert(!near_gray(color));
  const uint8_t mask[] = {1, 0, 0, 1, 1, 0};
  for (int scale : {2, 3, 4, 6}) {
    const int width = 3 * scale, height = 2 * scale, stride = width * 4 + 12;
    std::vector<uint8_t> pixels(stride * height, 77);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
      auto *p = pixels.data() + y * stride + x * 4;
      p[0] = 70; p[1] = 100; p[2] = 130;
    }
    neutralize(pixels.data(), width, height, stride, mask, 3, 2);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
      const auto *p = pixels.data() + y * stride + x * 4;
      if (mask[(y / scale) * 3 + x / scale]) assert(p[0] == 94 && p[1] == 94 && p[2] == 94);
      else assert(p[0] == 70 && p[1] == 100 && p[2] == 130);
      assert(p[3] == 77);
    }
  }
  std::array<uint16_t, 256> table{};
  for (int i = 0; i < 256; ++i) table[i] = i;
  std::vector<uint8_t> flat(7 * 9 * 4, 255);
  std::vector<uint8_t> tensor(12 * 12 * 3);
  int coordinates[12];
  assert(pack_tile(flat.data(), 7, 9, 28, -3, -3, 12, table.data(), tensor.data(), coordinates) == 0xffffff);
  flat[(4 * 7 + 2) * 4] = 254;
  assert(pack_tile(flat.data(), 7, 9, 28, -3, -3, 12, table.data(), tensor.data(), coordinates) == -1);
  UniformTileCache cache;
  std::vector<uint8_t> model_output(1 << 20, 123);
  cache.remember(0xffffff, model_output.data(), model_output.size());
  assert(cache.find(0xffffff, model_output.size())[123] == 123);
  assert(cache.find(0, model_output.size()) == nullptr);
  for (int color = 0; color < 20; ++color) cache.remember(color, model_output.data(), model_output.size());
  assert(cache.bytes() <= UniformTileCache::MAX_BYTES);
  assert(cache.find(0xffffff, model_output.size()) == nullptr);
  cache.clear();
  assert(cache.find(19, model_output.size()) == nullptr);
}

static void fp16_and_int8() {
  assert(half_to_float(1) == std::ldexp(1.0f, -24));
  assert(half_to_float(0x3ff) == 1023 * std::ldexp(1.0f, -24));
  assert(half_to_float(0x400) == std::ldexp(1.0f, -14));
  assert(half_to_float(0x3c00) == 1.0f);
  assert(half_to_float(0xbc00) == -1.0f);
  assert(std::isinf(half_to_float(0x7c00)));
  assert(std::isnan(half_to_float(0x7e00)));
  ConversionTables tables;
  tables.build([](float v) { return std::clamp(std::lround(v * 200 + 7), 0L, 255L); },
               [](uint16_t v) { return (static_cast<int>(v) - 9) / 180.0f; }, 256);
  for (int i = 0; i < 256; ++i) {
    assert(tables.input[i] == std::clamp(std::lround((i / 255.0f) * 200 + 7), 0L, 255L));
    assert(tables.output[i] == static_cast<uint8_t>(std::clamp((i - 9) / 180.0f, 0.0f, 1.0f) * 255 + 0.5f));
  }
  const uint8_t input[] = {17, 42, 255, 133};
  uint8_t packed[3]; int coordinates[1];
  assert(pack_tile(input, 1, 1, 4, 0, 0, 1, tables.input.data(), packed, coordinates) >= 0);
  uint8_t output[16]{};
  const uint8_t tensor[] = {12, 50, 255, 12, 50, 255, 12, 50, 255, 12, 50, 255};
  unpack_tile(tensor, 2, 0, tables.output.data(), output, 8, 0, 0, 2, 2, input, 4, 2);
  for (int i = 0; i < 4; ++i) {
    assert(output[i * 4] == tables.output[12]);
    assert(output[i * 4 + 1] == tables.output[50]);
    assert(output[i * 4 + 2] == 255 && output[i * 4 + 3] == 133);
  }
}

static void benchmark() {
  constexpr int side = 512, scale = 2, output_side = side * scale;
  const int output_stride = output_side * 4;
  volatile float configured_scale = 0.00003125f;
  const float quant_scale = configured_scale;
  std::vector<uint8_t> input(side * side * 4, 255);
  std::vector<uint16_t> tensor(output_side * output_side * 3);
  std::mt19937 rng(131);
  for (auto &v : tensor) v = static_cast<uint16_t>(rng());
  std::vector<uint8_t> reference(output_stride * output_side), optimized(reference.size());
  ConversionTables tables;
  tables.build([](float v) { return static_cast<uint16_t>(v * 255); },
               [&](uint16_t v) { return (static_cast<int>(v) - 100) * quant_scale; }, 65536);
  auto run = [](auto fn) {
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; ++i) fn();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 20;
  };
  const auto old_ms = run([&] {
    for (int y = 0; y < output_side; ++y) for (int x = 0; x < output_side; ++x) {
      for (int c = 0; c < 3; ++c) {
        const float value = std::clamp((static_cast<int>(tensor[(y * output_side + x) * 3 + c]) - 100) * quant_scale, 0.0f, 1.0f);
        reference[y * output_stride + x * 4 + c] = static_cast<uint8_t>(value * 255.0f + 0.5f);
      }
      reference[y * output_stride + x * 4 + 3] = input[((y / scale) * side + x / scale) * 4 + 3];
    }
  });
  const auto new_ms = run([&] {
    unpack_tile(tensor.data(), output_side, 0, tables.output.data(), optimized.data(),
                 output_stride, 0, 0, output_side, output_side, input.data(), side * 4, scale);
  });
  assert(reference == optimized);
  std::cout << "Host CPU conversion only, 1.05 MP, 20 iterations: reference=" << old_ms
            << "ms lookup=" << new_ms << "ms (not phone/NPU inference timing)\n";
}

int main() {
  conversion_and_edges();
  output_geometry_and_alpha();
  gray_and_cache();
  fp16_and_int8();
  benchmark();
  std::cout << "Native pixel, alpha, border, gray-mask and cache checks passed.\n";
}
