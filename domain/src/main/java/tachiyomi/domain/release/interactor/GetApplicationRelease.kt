package tachiyomi.domain.release.interactor

import tachiyomi.core.common.preference.Preference
import tachiyomi.core.common.preference.PreferenceStore
import tachiyomi.domain.release.model.Release
import tachiyomi.domain.release.service.ReleaseService
import java.time.Instant
import java.time.temporal.ChronoUnit

class GetApplicationRelease(
    private val service: ReleaseService,
    private val preferenceStore: PreferenceStore,
) {

    private val lastChecked: Preference<Long> by lazy {
        preferenceStore.getLong(Preference.appStateKey("last_app_check"), 0)
    }

    suspend fun await(arguments: Arguments): Result {
        val now = Instant.now()

        // Limit checks to once every 3 days at most
        val nextCheckTime = Instant.ofEpochMilli(lastChecked.get()).plus(3, ChronoUnit.DAYS)
        if (!arguments.forceCheck && now.isBefore(nextCheckTime)) {
            return Result.NoNewUpdate
        }

        val release = service.latest(arguments.repository)

        lastChecked.set(now.toEpochMilli())

        // Check if latest version is different from current version
        // SY -->
        val isNewVersion =
            isNewVersion(arguments.isPreview, arguments.syDebugVersion, arguments.versionName, release.version)
        // SY <--
        return when {
            isNewVersion -> Result.NewUpdate(release)
            else -> Result.NoNewUpdate
        }
    }

    // SY -->
    internal fun isNewVersion(
        isPreview: Boolean,
        syDebugVersion: String,
        versionName: String,
        versionTag: String,
    ): Boolean {
        return if (isPreview) {
            val current = syDebugVersion.toLongOrNull() ?: return false
            val next = versionTag.removePrefix("r").removePrefix("v").toLongOrNull() ?: return false
            next > current
        } else {
            fun parse(value: String): List<Long>? {
                val match = Regex("^[vVrR]?(\\d+(?:\\.\\d+)*)(?:[-+].*)?$").matchEntire(value) ?: return null
                return match.groupValues[1].split('.').map { it.toLongOrNull() ?: return null }
            }
            val current = parse(versionName) ?: return false
            val next = parse(versionTag) ?: return false
            for (index in 0 until maxOf(current.size, next.size)) {
                val comparison = (next.getOrElse(index) { 0L }).compareTo(current.getOrElse(index) { 0L })
                if (comparison != 0) return comparison > 0
            }
            false
        }
    }
    // SY <--

    data class Arguments(
        val isPreview: Boolean,
        val commitCount: Int,
        val versionName: String,
        val repository: String,
        // SY -->
        val syDebugVersion: String,
        // SY <--
        val forceCheck: Boolean = false,
    )

    sealed interface Result {
        data class NewUpdate(val release: Release) : Result
        data object NoNewUpdate : Result
        data object OsTooOld : Result
    }
}
