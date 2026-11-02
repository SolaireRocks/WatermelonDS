package me.magnum.melonds.domain.model

import android.net.Uri
import java.util.*

data class SaveStateSlot(val slot: Int, val exists: Boolean, val lastUsedDate: Date?, val screenshot: Uri?) {

    val isQuickSlot: Boolean
        get() = slot == QUICK_SAVE_SLOT || slot in FIRST_ROLLING_SLOT until FIRST_ROLLING_SLOT + MAX_ROLLING_QUICK_SAVES - 1

    val isAutoSaveSlot: Boolean
        get() = slot == AUTO_SAVE_SLOT

    companion object {
        const val QUICK_SAVE_SLOT = 0
        const val AUTO_SAVE_SLOT = 9
        // The quick slot counts as the first rolling save. The rest use the slots starting from this one
        const val FIRST_ROLLING_SLOT = 10
        const val MAX_ROLLING_QUICK_SAVES = 10
    }
}
