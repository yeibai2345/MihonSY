package eu.kanade.tachiyomi.util.waifu2x

import org.junit.jupiter.api.Assertions.assertFalse
import org.junit.jupiter.api.Assertions.assertTrue
import org.junit.jupiter.api.Test

class EnhancementLoadPolicyTest {
    @Test fun `Visible pages remain eligible below critical even while scrolling`() {
        for (level in 0..3) for (priority in listOf(90, 100)) {
            assertTrue(EnhancementLoadPolicy.canStart(priority, level, false))
            assertTrue(EnhancementLoadPolicy.canStart(priority, level, true))
        }
    }
    @Test fun `Critical temperature pauses all new inference`() {
        for (level in 4..6) for (priority in listOf(0, 70, 80, 90, 100)) {
            assertFalse(EnhancementLoadPolicy.canStart(priority, level, false))
        }
    }
    @Test fun `Only nearby prefetch is admitted while cool and stationary`() {
        for (priority in listOf(70, 80)) {
            assertTrue(EnhancementLoadPolicy.canStart(priority, 0, false))
            assertFalse(EnhancementLoadPolicy.canStart(priority, 0, true))
            for (level in 1..3) assertFalse(EnhancementLoadPolicy.canStart(priority, level, false))
        }
        for (priority in listOf(0, 1, 69)) assertFalse(EnhancementLoadPolicy.canStart(priority, 0, false))
    }
}
