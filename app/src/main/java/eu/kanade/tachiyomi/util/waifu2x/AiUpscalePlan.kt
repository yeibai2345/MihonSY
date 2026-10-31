package eu.kanade.tachiyomi.util.waifu2x

/** Plans bounded AI passes without allocating a bitmap larger than [MAX_OUTPUT_PIXELS]. */
data class AiUpscalePlan(val scale: Int, val output: Size, val passes: List<Pass>) {
    data class Size(val width: Int, val height: Int) {
        val pixels: Long get() = width.toLong() * height
    }

    data class Pass(val input: Size, val output: Size)

    companion object {
        const val MAX_OUTPUT_PIXELS = 36_000_000L
        val SCALE_OPTIONS = (2..6).toList()

        fun normalizeScale(scale: Int): Int = scale.takeIf { it in SCALE_OPTIONS } ?: 2

        /** Large pages use the highest safe scale; if even 2x cannot fit, keep the original. */
        fun create(width: Int, height: Int, requestedScale: Int, modelScale: Int = 2): AiUpscalePlan? {
            if (width <= 0 || height <= 0 || modelScale !in 2..6) return null
            for (scale in normalizeScale(requestedScale) downTo 2) {
                val targetWidth = width.toLong() * scale
                val targetHeight = height.toLong() * scale
                if (targetWidth > Int.MAX_VALUE || targetHeight > Int.MAX_VALUE) continue
                val target = Size(targetWidth.toInt(), targetHeight.toInt())
                if (target.pixels > MAX_OUTPUT_PIXELS) continue

                val passes = mutableListOf<Pass>()
                var current = Size(width, height)
                while (current.width < target.width || current.height < target.height) {
                    // Resize BEFORE the final pass: 6x uses 2x -> 4x -> 3x -> 6x,
                    // avoiding an 8x bitmap and its extra memory cost.
                    val input = Size(
                        minOf(current.width, ((targetWidth + modelScale - 1) / modelScale).toInt()),
                        minOf(current.height, ((targetHeight + modelScale - 1) / modelScale).toInt()),
                    )
                    val output = Size(input.width * modelScale, input.height * modelScale)
                    if (output.pixels > MAX_OUTPUT_PIXELS) break
                    passes += Pass(input, output)
                    current = output
                }
                if (current.width >= target.width && current.height >= target.height) {
                    return AiUpscalePlan(scale, target, passes)
                }
            }
            return null
        }
    }
}
