package eu.kanade.tachiyomi.util.waifu2x

/** Source-space bands; overlapping context is discarded after inference. */
data class StripUpscalePlan(
    val width: Int,
    val height: Int,
    val scale: Int,
    val bandHeight: Int,
    val halo: Int,
) {
    data class Band(val index: Int, val top: Int, val bottom: Int, val inputTop: Int, val inputBottom: Int) {
        val height: Int get() = bottom - top
        val inputHeight: Int get() = inputBottom - inputTop
        val paddedInputHeight: Int get() = (inputHeight + 7) / 8 * 8
    }

    val count: Int get() = (height - 1) / bandHeight + 1

    fun band(index: Int): Band {
        require(index in 0 until count)
        val top = index * bandHeight
        val bottom = minOf(height, top + bandHeight)
        return Band(index, top, bottom, maxOf(0, top - halo), minOf(height, bottom + halo))
    }

    fun visibleBands(top: Int, bottom: Int): IntRange {
        val first = top.coerceIn(0, height - 1) / bandHeight
        val last = (bottom - 1).coerceIn(top.coerceIn(0, height - 1), height - 1) / bandHeight
        return first..last
    }

    companion object {
        fun create(width: Int, height: Int, scale: Int, modelScale: Int, padding: Int): StripUpscalePlan? {
            if (width <= 0 || height < 1024 || height.toLong() * 2 <= width.toLong() * 5 ||
                scale !in 2..6 || modelScale != 2 || padding !in 0..48
            ) return null
            // Eight-pixel alignment preserves the 3x path's half-pixel phase across bands.
            var passes = 1
            var achieved = modelScale
            while (achieved < scale) { achieved *= modelScale; passes++ }
            val halo = ((padding * passes + 8 + 7) / 8) * 8
            val maxHeight = (AiUpscalePlan.MAX_OUTPUT_PIXELS / (width.toLong() * scale * scale)).toInt()
            val core = minOf(512, (maxHeight - 2 * halo) / 8 * 8)
            if (core < 128) return null
            val candidate = AiUpscalePlan.create(width, minOf(height, core + 2 * halo), scale, modelScale)
            if (candidate?.scale != scale) return null
            return StripUpscalePlan(width, height, scale, core, halo)
        }
    }
}
