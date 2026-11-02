package me.magnum.melonds.domain.repositories

import android.graphics.Bitmap
import android.net.Uri
import me.magnum.melonds.domain.model.rom.Rom
import me.magnum.melonds.domain.model.SaveStateSlot

interface SaveStatesRepository {
    fun getRomSaveStates(rom: Rom): List<SaveStateSlot>
    /** The slots that can be loaded: the existing quick saves (newest first), the autosave if it exists and the manual slots. */
    fun getRomLoadableSaveStates(rom: Rom): List<SaveStateSlot>
    /** The existing quick saves, newest first. Contains just the empty quick slot if there are none. */
    fun getRomQuickSaveStateSlots(rom: Rom): List<SaveStateSlot>
    /** The slot the next quick save should be written to. */
    fun getRomNextQuickSaveStateSlot(rom: Rom): SaveStateSlot
    fun getRomSaveStateUri(rom: Rom, saveState: SaveStateSlot): Uri
    fun setRomSaveStateScreenshot(rom: Rom, saveState: SaveStateSlot, screenshot: Bitmap)
    fun deleteRomSaveStateScreenshot(rom: Rom, saveState: SaveStateSlot)
    fun deleteRomSaveState(rom: Rom, saveState: SaveStateSlot)
}
