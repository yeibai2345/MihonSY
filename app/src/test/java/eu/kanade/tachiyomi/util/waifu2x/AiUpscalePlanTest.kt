package eu.kanade.tachiyomi.util.waifu2x

import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertNotNull
import org.junit.jupiter.api.Assertions.assertNull
import org.junit.jupiter.api.Assertions.assertTrue
import org.junit.jupiter.api.Test

class AiUpscalePlanTest {
    @Test
    fun `All scales reach the requested dimensions without an oversized intermediate`() {
        for (scale in 2..6) {
            val plan = requireNotNull(AiUpscalePlan.create(501, 777, scale))
            assertEquals(scale, plan.scale)
            assertEquals(AiUpscalePlan.Size(501 * scale, 777 * scale), plan.output)
            assertTrue(plan.passes.all { it.output.pixels <= AiUpscalePlan.MAX_OUTPUT_PIXELS })
            assertTrue(plan.passes.size in 1..3)
            assertTrue(plan.passes.last().output.width >= plan.output.width)
            assertTrue(plan.passes.last().output.height >= plan.output.height)
        }
    }

    @Test
    fun `6x stays within the cap without first allocating 8x`() {
        val plan = requireNotNull(AiUpscalePlan.create(1000, 1000, 6))
        assertEquals(listOf(2000, 4000, 6000), plan.passes.map { it.output.width })
        assertEquals(AiUpscalePlan.Size(3000, 3000), plan.passes.last().input)
        assertEquals(36_000_000L, plan.output.pixels)
    }

    @Test
    fun `Large pages reduce the scale or retain the original`() {
        assertEquals(4, AiUpscalePlan.create(1200, 1800, 6)?.scale)
        assertEquals(2, AiUpscalePlan.create(800, 10000, 6)?.scale)
        assertNull(AiUpscalePlan.create(4000, 3000, 6))
        assertNotNull(AiUpscalePlan.create(3000, 3000, 2))
    }

    @Test
    fun `Invalid dimensions and preferences cannot overflow the planner`() {
        assertNull(AiUpscalePlan.create(0, 100, 6))
        assertNull(AiUpscalePlan.create(-1, 100, 6))
        assertNull(AiUpscalePlan.create(Int.MAX_VALUE, Int.MAX_VALUE, 6))
        assertNull(AiUpscalePlan.create(100, 100, 6, 1))
        assertEquals(2, AiUpscalePlan.create(100, 100, 99)?.scale)
    }

    @Test
    fun `Models with other native scales also receive bounded passes`() {
        for (modelScale in 2..6) {
            for (scale in 2..6) {
                val plan = requireNotNull(AiUpscalePlan.create(101, 99, scale, modelScale))
                assertEquals(scale, plan.scale)
                assertTrue(plan.passes.all { it.output.width == it.input.width * modelScale })
                assertTrue(plan.passes.all { it.output.height == it.input.height * modelScale })
            }
        }
    }
}
