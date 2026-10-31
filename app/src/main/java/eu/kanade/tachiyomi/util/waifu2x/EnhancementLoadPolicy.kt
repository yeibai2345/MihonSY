package eu.kanade.tachiyomi.util.waifu2x

/** Android thermal status values: NONE=0, LIGHT=1, MODERATE=2, SEVERE=3, CRITICAL=4. */
internal object EnhancementLoadPolicy {
    fun canStart(priority: Int, thermalStatus: Int, scrolling: Boolean): Boolean {
        if (thermalStatus >= 4) return false
        if (priority >= 90) return true
        return priority >= 70 && thermalStatus <= 0 && !scrolling
    }
}
