package eu.kanade.tachiyomi.ui.reader.viewer.webtoon

import android.graphics.Bitmap
import android.util.LruCache
import eu.kanade.tachiyomi.util.waifu2x.Waifu2x

/** Bounded, session-only results. A displayed bitmap can outlive its cache entry. */
internal object WebtoonImageCache {
    data class Entry(val bitmap: Bitmap, val elapsedMillis: Long, val engine: Waifu2x.EngineKind)

    private val budget = (Runtime.getRuntime().maxMemory() / 4).coerceIn(16L shl 20, 128L shl 20).toInt()
    private val cache = object : LruCache<String, Entry>(budget) {
        override fun sizeOf(key: String, value: Entry): Int = value.bitmap.allocationByteCount
        // Never recycle on eviction: SSIV may still be drawing the same bitmap.
    }

    fun get(key: String): Entry? = cache.get(key)?.takeUnless { it.bitmap.isRecycled }

    fun put(key: String, entry: Entry) {
        if (!entry.bitmap.isRecycled && entry.bitmap.allocationByteCount <= budget) cache.put(key, entry)
    }

    fun clear() = cache.evictAll()
}
