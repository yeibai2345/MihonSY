package eu.kanade.tachiyomi.util.waifu2x

import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertNull
import org.junit.jupiter.api.Assertions.assertTrue
import org.junit.jupiter.api.Test

class StripUpscalePlanTest {
    @Test
    fun `Long strips retain 3x and 4x without allocating the whole output`() {
        for (scale in 3..4) {
            val plan = requireNotNull(StripUpscalePlan.create(800, 10_001, scale, 2, 35))
            assertEquals(scale, plan.scale)
            assertTrue(800L * 10_001 * scale * scale > AiUpscalePlan.MAX_OUTPUT_PIXELS)
            var nextTop = 0
            for (index in 0 until plan.count) {
                val band = plan.band(index)
                assertEquals(nextTop, band.top)
                assertTrue(band.inputTop <= band.top && band.inputBottom >= band.bottom)
                assertTrue(band.inputTop >= 0 && band.inputBottom <= plan.height)
                assertEquals(0, band.inputTop % 8)
                assertEquals(0, band.paddedInputHeight % 8)
                assertTrue(band.paddedInputHeight - band.inputHeight in 0..7)
                val inference = requireNotNull(AiUpscalePlan.create(plan.width, band.paddedInputHeight, scale))
                assertEquals(scale, inference.scale)
                assertTrue(inference.passes.all { it.output.pixels <= AiUpscalePlan.MAX_OUTPUT_PIXELS })
                nextTop = band.bottom
            }
            assertEquals(plan.height, nextTop)
        }
    }

    @Test
    fun `Visible windows include exactly intersecting bands`() {
        val plan = requireNotNull(StripUpscalePlan.create(800, 10_000, 3, 2, 35))
        assertEquals(0..0, plan.visibleBands(0, plan.bandHeight))
        assertEquals(1..1, plan.visibleBands(plan.bandHeight, plan.bandHeight * 2))
        assertEquals(0..1, plan.visibleBands(plan.bandHeight - 1, plan.bandHeight + 1))
        assertEquals(0..0, plan.visibleBands(-10, -1))
        assertEquals(plan.count - 1..plan.count - 1, plan.visibleBands(99_999, 100_001))
    }

    @Test
    fun `All supported scales and source widths respect the per-band budget`() {
        for (width in listOf(101, 800, 1200, 2048, 4096)) {
            for (scale in 2..6) {
                val plan = StripUpscalePlan.create(width, width * 10 + 1, scale, 2, 48) ?: continue
                for (index in 0 until plan.count) {
                    val band = plan.band(index)
                    assertEquals(scale, AiUpscalePlan.create(width, band.paddedInputHeight, scale)?.scale)
                    if (band.top > plan.halo) assertTrue(band.top - band.inputTop >= plan.halo)
                    if (band.bottom + plan.halo < plan.height) assertTrue(band.inputBottom - band.bottom >= plan.halo)
                }
            }
        }
    }

    @Test
    fun `Invalid inputs and unsupported model phases use the existing decoder`() {
        assertNull(StripUpscalePlan.create(0, 10000, 3, 2, 35))
        assertNull(StripUpscalePlan.create(1000, 1000, 3, 2, 35))
        assertNull(StripUpscalePlan.create(1000, 10000, 3, 3, 35))
        assertNull(StripUpscalePlan.create(1000, 10000, 3, 2, 100))
        assertNull(StripUpscalePlan.create(Int.MAX_VALUE, Int.MAX_VALUE, 6, 2, 35))
    }
}
