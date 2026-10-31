package eu.kanade.tachiyomi.util

/** Bounded processing records for diagnostics, independent of image/result caches. */
object EnhancementDiagnostics {
    private val records = java.util.ArrayDeque<String>()

    @Synchronized
    fun record(message: String) {
        if (records.size >= 64) records.removeFirst()
        records.addLast("${System.currentTimeMillis()} $message")
        android.util.Log.d("EnhancementProfile", message)
    }

    @Synchronized
    fun snapshot(): String = records.joinToString("\n")
}
