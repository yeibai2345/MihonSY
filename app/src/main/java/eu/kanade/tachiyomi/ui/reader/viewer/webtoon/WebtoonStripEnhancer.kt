package eu.kanade.tachiyomi.ui.reader.viewer.webtoon

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Rect
import android.graphics.RectF
import android.os.SystemClock
import eu.kanade.tachiyomi.ui.reader.setting.ReaderPreferences
import eu.kanade.tachiyomi.util.EnhanceTimings
import eu.kanade.tachiyomi.util.MihonSyEnhancer
import eu.kanade.tachiyomi.util.waifu2x.StripUpscalePlan
import eu.kanade.tachiyomi.util.waifu2x.Waifu2x
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlin.math.abs

/** Draws completed AI bands over the original, without changing the scroll geometry. */
internal class WebtoonStripEnhancer(
    private val view: WebtoonSubsamplingImageView,
    private val source: Bitmap,
    private val plan: StripUpscalePlan,
    private val preferences: ReaderPreferences,
    private val pageId: Int,
    private val cacheIdentity: String,
    private val onVisiblePending: () -> Unit,
    private val onVisibleEnhanced: (Long, Waifu2x.EngineKind) -> Unit,
) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private val paint = Paint(Paint.FILTER_BITMAP_FLAG)
    private val visibleRect = Rect()
    private val destination = RectF()
    private val tiles = mutableMapOf<Int, WebtoonImageCache.Entry>()
    private val attempts = mutableMapOf<Int, Int>()
    private val lastAttempt = mutableMapOf<Int, Long>()
    private val fingerprint = preferences.enhancementCacheKey()
    @Volatile private var closed = false
    @Volatile private var wanted = emptySet<Int>()
    @Volatile private var visible = emptyList<Int>()
    private var ordered = emptyList<Int>()
    private var previousTop = 0
    private var direction = 1
    private var activeBand: Int? = null
    private var work: Job? = null
    private var reportedVisible = emptyList<Int>()

    fun close() {
        closed = true
        wanted = emptySet()
        scope.cancel()
        tiles.clear()
    }

    fun updateViewport() {
        if (closed || !view.isReady) return
        if (!view.getLocalVisibleRect(visibleRect) || !view.isShown) {
            wanted = emptySet()
            visible = emptyList()
            ordered = emptyList()
            tiles.clear()
            return
        }
        val top = view.viewToSourceCoord(0f, visibleRect.top.toFloat())?.y?.toInt() ?: return
        val bottom = view.viewToSourceCoord(0f, visibleRect.bottom.toFloat())?.y?.toInt() ?: return
        if (top != previousTop) direction = if (top > previousTop) 1 else -1
        previousTop = top
        val range = plan.visibleBands(top, bottom)
        visible = range.toList()
        val center = (range.first + range.last) / 2f
        val next = if (direction > 0) range.last + 1 else range.first - 1
        ordered = visible.sortedBy { abs(it - center) } + listOf(next).filter { it in 0 until plan.count }
        wanted = ordered.toSet()
        tiles.keys.retainAll(wanted)
        for (index in ordered) {
            if (index !in tiles) WebtoonImageCache.get(key(index))?.let { tiles[index] = it }
        }
        schedule()
        reportVisible()
    }

    fun draw(canvas: Canvas) {
        updateViewport()
        for (index in visible) {
            val entry = tiles[index] ?: continue
            if (entry.bitmap.isRecycled) continue
            val band = plan.band(index)
            val top = view.sourceToViewCoord(0f, band.top.toFloat()) ?: continue
            val bottom = view.sourceToViewCoord(plan.width.toFloat(), band.bottom.toFloat()) ?: continue
            destination.set(top.x, top.y, bottom.x, bottom.y)
            canvas.drawBitmap(entry.bitmap, null, destination, paint)
        }
    }

    private fun key(index: Int): String = "$cacheIdentity|strip-v1|${source.width}x${source.height}|$fingerprint|$index"

    private fun schedule() {
        if (closed || activeBand != null) return
        val now = SystemClock.uptimeMillis()
        val index = ordered.firstOrNull {
            it !in tiles && attempts.getOrDefault(it, 0) < 2 &&
                now - lastAttempt.getOrDefault(it, -10_000L) >= 1_000L
        } ?: return
        activeBand = index
        attempts[index] = attempts.getOrDefault(index, 0) + 1
        lastAttempt[index] = now
        val displayWidth = view.width
        work = scope.launch(Dispatchers.IO) {
            val result = enhance(index, displayWidth)
            // Always reclaim an abandoned result, even if the view was recycled
            // while native inference was completing on this worker.
            withContext(NonCancellable + Dispatchers.Main.immediate) {
                activeBand = null
                work = null
                if (closed || index !in wanted) {
                    result?.bitmap?.recycle()
                    // Leaving the viewport is not an inference failure. Allow a later revisit.
                    attempts.remove(index)
                    lastAttempt.remove(index)
                } else if (result != null) {
                    tiles[index] = result
                    WebtoonImageCache.put(key(index), result)
                    view.invalidate()
                } else if (attempts.getOrDefault(index, 0) < 2) {
                    view.postDelayed({ if (!closed) updateViewport() }, 1_050L)
                }
                if (!closed) {
                    schedule()
                    reportVisible()
                }
            }
        }
    }

    private fun enhance(index: Int, displayWidth: Int): WebtoonImageCache.Entry? {
        val band = plan.band(index)
        var input: Bitmap? = null
        var output: Bitmap? = null
        val cancelled = {
            closed || index !in wanted || preferences.enhancementCacheKey() != fingerprint
        }
        try {
            if (cancelled() || source.isRecycled) return null
            // Align the final band's height too: a 3x pass must use exactly 1.5x
            // intermediate scaling on every band. Reflect the few extra edge rows.
            input = Bitmap.createBitmap(plan.width, band.paddedInputHeight, Bitmap.Config.ARGB_8888)
            val canvas = Canvas(requireNotNull(input))
            canvas.drawBitmap(source, Rect(0, band.inputTop, plan.width, band.inputBottom),
                Rect(0, 0, plan.width, band.inputHeight), null)
            for (row in band.inputHeight until band.paddedInputHeight) {
                val sourceRow = band.inputBottom - 2 - (row - band.inputHeight)
                canvas.drawBitmap(source, Rect(0, sourceRow, plan.width, sourceRow + 1),
                    Rect(0, row, plan.width, row + 1), null)
            }
            val bitmap = input ?: return null
            val start = SystemClock.uptimeMillis()
            var waiting = 0L
            output = MihonSyEnhancer.enhance(
                bitmap, preferences,
                onComplete = { _, _, wait -> waiting = wait },
                sourceTag = "webtoon-strip#$pageId:$index",
                pageIndex = pageId,
                shouldCancel = cancelled,
                priorityOffset = { if (index in visible) 0 else -40 },
            )
            val enhanced = output ?: return null
            if (cancelled() || enhanced.width != plan.width * plan.scale ||
                enhanced.height != band.paddedInputHeight * plan.scale
            ) return null
            val core = Bitmap.createBitmap(
                enhanced, 0, (band.top - band.inputTop) * plan.scale,
                plan.width * plan.scale, band.height * plan.scale,
            )
            if (core !== enhanced) enhanced.recycle()
            output = core
            if (preferences.aiAreaDownscale.get()) {
                val display = MihonSyEnhancer.areaDownscaleToDisplay(
                    core, viewWidth = displayWidth, viewHeight = 0, isTallStrip = true,
                    strength = preferences.aiAreaDownscaleStrength.get() / 100f,
                )
                if (display !== core) core.recycle()
                output = display
            }
            if (cancelled()) return null
            val result = WebtoonImageCache.Entry(
                requireNotNull(output), (SystemClock.uptimeMillis() - start - waiting).coerceAtLeast(0L),
                Waifu2x.engineFor(pageId),
            )
            output = null
            return result
        } catch (_: Exception) {
            return null
        } catch (_: OutOfMemoryError) {
            return null
        } finally {
            output?.takeUnless { it.isRecycled || it === source }?.recycle()
            input?.takeUnless { it.isRecycled || it === source }?.recycle()
            EnhanceTimings.takeSkipped(pageId)
            EnhanceTimings.takeInterrupted(pageId)
        }
    }

    private fun reportVisible() {
        if (visible.isEmpty() || visible.any { it !in tiles }) {
            if (reportedVisible.isNotEmpty()) {
                reportedVisible = emptyList()
                onVisiblePending()
            }
            return
        }
        if (visible == reportedVisible) return
        reportedVisible = visible.toList()
        val entries = visible.mapNotNull(tiles::get)
        onVisibleEnhanced(entries.sumOf { it.elapsedMillis }, entries.last().engine)
    }
}
