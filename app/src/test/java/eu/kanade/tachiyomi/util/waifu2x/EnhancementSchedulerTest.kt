package eu.kanade.tachiyomi.util.waifu2x

import java.util.Collections
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertNull
import org.junit.jupiter.api.Assertions.assertThrows
import org.junit.jupiter.api.Assertions.assertTrue
import org.junit.jupiter.api.Test
import org.junit.jupiter.api.Timeout

@Timeout(10)
class EnhancementSchedulerTest {
    private fun awaitQueue(scheduler: EnhancementScheduler, count: Int) {
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(3)
        while (scheduler.pendingCount != count && System.nanoTime() < deadline) Thread.sleep(1)
        assertEquals(count, scheduler.pendingCount)
    }

    @Test
    fun `Waiting jobs follow the current visible page rather than arrival priority`() {
        val scheduler = EnhancementScheduler()
        val workers = Executors.newFixedThreadPool(3)
        val active = CountDownLatch(1)
        val release = CountDownLatch(1)
        val visible = AtomicInteger(1)
        val order = Collections.synchronizedList(mutableListOf<Int>())
        try {
            val first = workers.submit { scheduler.run({ 0 }) { active.countDown(); release.await() } }
            assertTrue(active.await(3, TimeUnit.SECONDS))
            val one = workers.submit { scheduler.run({ if (visible.get() == 1) 100 else 0 }) { order += 1 } }
            awaitQueue(scheduler, 1)
            val two = workers.submit { scheduler.run({ if (visible.get() == 2) 100 else 0 }) { order += 2 } }
            awaitQueue(scheduler, 2)
            visible.set(2)
            scheduler.prioritiesChanged()
            release.countDown()
            first.get(3, TimeUnit.SECONDS)
            one.get(3, TimeUnit.SECONDS)
            two.get(3, TimeUnit.SECONDS)
            assertEquals(listOf(2, 1), order)
        } finally {
            release.countDown()
            workers.shutdownNow()
        }
    }

    @Test
    fun `Cancellation removes a queued request without entering the engine`() {
        val scheduler = EnhancementScheduler()
        val workers = Executors.newFixedThreadPool(2)
        val active = CountDownLatch(1)
        val release = CountDownLatch(1)
        val cancelled = AtomicBoolean(false)
        val invoked = AtomicBoolean(false)
        try {
            val first = workers.submit { scheduler.run({ 0 }) { active.countDown(); release.await() } }
            assertTrue(active.await(3, TimeUnit.SECONDS))
            val queued = workers.submit<Boolean?> { scheduler.run({ 100 }, cancelled::get) { invoked.set(true); true } }
            awaitQueue(scheduler, 1)
            cancelled.set(true)
            scheduler.prioritiesChanged()
            assertNull(queued.get(3, TimeUnit.SECONDS))
            assertTrue(!invoked.get())
            release.countDown()
            first.get(3, TimeUnit.SECONDS)
            assertEquals(0, scheduler.pendingCount)
        } finally {
            release.countDown()
            workers.shutdownNow()
        }
    }

    @Test
    fun `A failed operation releases the engine slot`() {
        val scheduler = EnhancementScheduler()
        assertThrows(IllegalStateException::class.java) { scheduler.run({ 0 }) { error("native failure") } }
        assertEquals(42, scheduler.run({ 0 }) { 42 })
    }
}
