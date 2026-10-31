#include <android/bitmap.h>
#include <jni.h>
#include <climits>
#include <vector>
#include "upscale_pixels.h"

// Bitmap pixels stay in native memory. Avoid getPixels()/setPixels() and the
// full-size Java IntArray (144 MB for a 36 MP 3x/4x result).
extern "C" JNIEXPORT jbyteArray JNICALL
Java_eu_kanade_tachiyomi_util_MihonSyEnhancer_nativeClampGray(
    JNIEnv *env, jobject, jobject bitmap) {
  AndroidBitmapInfo info{};
  if (AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS ||
      info.format != ANDROID_BITMAP_FORMAT_RGBA_8888 || !info.width || !info.height)
    return nullptr;
  const size_t count = static_cast<size_t>(info.width) * info.height;
  if (count > INT_MAX) return nullptr;
  void *pixels = nullptr;
  if (AndroidBitmap_lockPixels(env, bitmap, &pixels) != ANDROID_BITMAP_RESULT_SUCCESS)
    return nullptr;
  jbyteArray result = nullptr;
  try {
    std::vector<uint8_t> mask;
    for (uint32_t y = 0; y < info.height; ++y) {
      auto *row = static_cast<uint8_t *>(pixels) + static_cast<size_t>(y) * info.stride;
      for (uint32_t x = 0; x < info.width; ++x) {
        auto *pixel = row + x * 4;
        if (upscale_pixels::near_gray(pixel)) {
          if (mask.empty()) mask.resize(count, 0);
          mask[static_cast<size_t>(y) * info.width + x] = 1;
          pixel[0] = pixel[1] = pixel[2] = upscale_pixels::luminance(pixel);
        }
      }
    }
    if (!mask.empty()) {
      result = env->NewByteArray(static_cast<jsize>(count));
      if (result)
        env->SetByteArrayRegion(result, 0, static_cast<jsize>(count),
                                reinterpret_cast<const jbyte *>(mask.data()));
    }
  } catch (const std::bad_alloc &) {
    // Enhancement can continue without the optional gray mask.
  }
  AndroidBitmap_unlockPixels(env, bitmap);
  return result;
}

extern "C" JNIEXPORT void JNICALL
Java_eu_kanade_tachiyomi_util_MihonSyEnhancer_nativeNeutralizeChroma(
    JNIEnv *env, jobject, jobject bitmap, jbyteArray mask, jint mask_width) {
  if (!mask || mask_width <= 0) return;
  const jsize count = env->GetArrayLength(mask);
  if (count <= 0 || count % mask_width != 0) return;
  AndroidBitmapInfo info{};
  if (AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS ||
      info.format != ANDROID_BITMAP_FORMAT_RGBA_8888 || !info.width || !info.height)
    return;
  jbyte *mask_pixels = env->GetByteArrayElements(mask, nullptr);
  if (!mask_pixels) return;
  void *pixels = nullptr;
  if (AndroidBitmap_lockPixels(env, bitmap, &pixels) == ANDROID_BITMAP_RESULT_SUCCESS) {
    upscale_pixels::neutralize(static_cast<uint8_t *>(pixels), info.width, info.height,
                               info.stride, reinterpret_cast<const uint8_t *>(mask_pixels),
                               mask_width, count / mask_width);
    AndroidBitmap_unlockPixels(env, bitmap);
  }
  env->ReleaseByteArrayElements(mask, mask_pixels, JNI_ABORT);
}
