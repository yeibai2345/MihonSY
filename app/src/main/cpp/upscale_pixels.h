#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace upscale_pixels {

inline int reflect(int coordinate, int size) {
  if (size <= 1) return 0;
  if (coordinate >= 0 && coordinate < size) return coordinate;
  const int64_t period = 2LL * (size - 1);
  int64_t value = coordinate % period;
  if (value < 0) value += period;
  return static_cast<int>(value < size ? value : period - value);
}

inline uint8_t output_byte(float value) {
  // Also makes corrupt/non-finite tensor values deterministic.
  if (!(value > 0.0f)) return 0;
  if (value >= 1.0f) return 255;
  return static_cast<uint8_t>(value * 255.0f + 0.5f);
}

// RGB inputs have only 256 possible channel values; quantized/FP16 outputs
// have at most 65536. Build these once per model, not once per output pixel.
struct ConversionTables {
  std::array<uint16_t, 256> input{};
  std::array<uint8_t, 65536> output{};

  template <typename Encode, typename Decode>
  void build(Encode encode, Decode decode, size_t output_values) {
    for (size_t i = 0; i < input.size(); ++i)
      input[i] = encode(static_cast<float>(i) / 255.0f);
    for (size_t i = 0; i < output_values; ++i)
      output[i] = output_byte(decode(static_cast<uint16_t>(i)));
  }
};

template <typename T>
int pack_tile(const uint8_t *input, int width, int height, int stride,
               int origin_x, int origin_y, int side, const uint16_t *table,
               T *destination, int *x_coordinates) {
  for (int x = 0; x < side; ++x)
    x_coordinates[x] = reflect(origin_x + x, width) * 4;
  const auto *first = input + static_cast<size_t>(reflect(origin_y, height)) * stride + x_coordinates[0];
  const int color = first[0] | (first[1] << 8) | (first[2] << 16);
  unsigned difference = 0;
  for (int y = 0; y < side; ++y) {
    const auto *row = input + static_cast<size_t>(reflect(origin_y + y, height)) * stride;
    for (int x = 0; x < side; ++x) {
      const auto *pixel = row + x_coordinates[x];
      difference |= (pixel[0] ^ first[0]) | (pixel[1] ^ first[1]) | (pixel[2] ^ first[2]);
      *destination++ = static_cast<T>(table[pixel[0]]);
      *destination++ = static_cast<T>(table[pixel[1]]);
      *destination++ = static_cast<T>(table[pixel[2]]);
    }
  }
  return difference == 0 ? color : -1;
}

// Only exact uniform RGB tiles qualify. Cache the model's actual tensor,
// including its border response; never synthesize a flat output or skip details.
class UniformTileCache {
 public:
  static constexpr size_t MAX_BYTES = 4 * 1024 * 1024;
  const uint8_t *find(int color, size_t size) {
    if (color < 0) return nullptr;
    for (auto &entry : entries_) {
      if (entry.color == color && entry.bytes.size() == size) {
        entry.used = ++clock_;
        return entry.bytes.data();
      }
    }
    return nullptr;
  }
  void remember(int color, const void *data, size_t size) {
    if (color < 0 || size > MAX_BYTES || !size) return;
    while (!entries_.empty() && (bytes_ + size > MAX_BYTES || entries_.size() >= 8)) {
      const auto oldest = std::min_element(entries_.begin(), entries_.end(),
          [](const Entry &a, const Entry &b) { return a.used < b.used; });
      bytes_ -= oldest->bytes.size();
      entries_.erase(oldest);
    }
    Entry entry{color, ++clock_, std::vector<uint8_t>(size)};
    std::memcpy(entry.bytes.data(), data, size);
    entries_.push_back(std::move(entry));
    bytes_ += size;
  }
  void clear() { entries_.clear(); bytes_ = 0; }
  size_t bytes() const { return bytes_; }
 private:
  struct Entry { int color; uint64_t used; std::vector<uint8_t> bytes; };
  std::vector<Entry> entries_;
  size_t bytes_ = 0;
  uint64_t clock_ = 0;
};

template <typename T>
void unpack_tile(const T *source, int source_side, int source_offset,
                 const uint8_t *table, uint8_t *output, int output_stride,
                 int target_x, int target_y, int copy_width, int copy_height,
                 const uint8_t *input, int input_stride, int scale) {
  for (int y = 0; y < copy_height; ++y) {
    auto *row = output + static_cast<size_t>(target_y + y) * output_stride + target_x * 4;
    const auto *source_row = source +
        (static_cast<size_t>(source_offset + y) * source_side + source_offset) * 3;
    const auto *alpha_row = input + static_cast<size_t>((target_y + y) / scale) * input_stride;
    for (int x = 0; x < copy_width; ++x) {
      row[x * 4] = table[source_row[x * 3]];
      row[x * 4 + 1] = table[source_row[x * 3 + 1]];
      row[x * 4 + 2] = table[source_row[x * 3 + 2]];
      row[x * 4 + 3] = alpha_row[((target_x + x) / scale) * 4 + 3];
    }
  }
}

inline bool near_gray(const uint8_t *pixel) {
  return std::max({pixel[0], pixel[1], pixel[2]}) -
      std::min({pixel[0], pixel[1], pixel[2]}) <= 8;
}

inline uint8_t luminance(const uint8_t *pixel) {
  return static_cast<uint8_t>((pixel[0] * 299 + pixel[1] * 587 + pixel[2] * 114) / 1000);
}

inline void neutralize(uint8_t *output, int width, int height, int stride,
                        const uint8_t *mask, int mask_width, int mask_height) {
  for (int y = 0; y < height; ++y) {
    auto *row = output + static_cast<size_t>(y) * stride;
    const auto *mask_row = mask + static_cast<size_t>(static_cast<int64_t>(y) * mask_height / height) * mask_width;
    for (int x = 0; x < width; ++x) {
      if (mask_row[static_cast<int64_t>(x) * mask_width / width]) {
        auto *pixel = row + x * 4;
        pixel[0] = pixel[1] = pixel[2] = luminance(pixel);
      }
    }
  }
}

} // namespace upscale_pixels
