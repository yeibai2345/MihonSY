package eu.kanade.tachiyomi.util.waifu2x

/** One native engine, with priorities re-evaluated as the reader scrolls. */
internal class EnhancementScheduler {
    private class Request(val priority: () -> Int, val cancelled: () -> Boolean)

    private val monitor = Object()
    private val pending = mutableListOf<Request>()
    private var running = false

    internal val pendingCount: Int get() = synchronized(monitor) { pending.size }

    fun prioritiesChanged() = synchronized(monitor) { monitor.notifyAll() }

    fun <T> run(
        priority: () -> Int,
        cancelled: () -> Boolean = { Thread.currentThread().isInterrupted },
        block: () -> T,
    ): T? {
        val request = Request(priority, cancelled)
        synchronized(monitor) {
            pending += request
            try {
                while (true) {
                    if (cancelled() || Thread.currentThread().isInterrupted) return null
                    // maxByOrNull retains FIFO order when priorities are equal.
                    val next = pending.filterNot { it.cancelled() }.maxByOrNull { it.priority() }
                    if (!running && next === request) {
                        running = true
                        break
                    }
                    try {
                        monitor.wait(50L)
                    } catch (e: InterruptedException) {
                        Thread.currentThread().interrupt()
                        return null
                    }
                }
            } finally {
                pending.remove(request)
                monitor.notifyAll()
            }
        }
        try {
            return if (cancelled()) null else block()
        } finally {
            synchronized(monitor) {
                running = false
                monitor.notifyAll()
            }
        }
    }
}
