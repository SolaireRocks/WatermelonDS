package me.magnum.melonds.impl

import android.content.Context
import android.graphics.Bitmap
import android.net.Uri
import android.provider.DocumentsContract
import androidx.documentfile.provider.DocumentFile
import me.magnum.melonds.common.uridelegates.UriHandler
import me.magnum.melonds.domain.model.rom.Rom
import me.magnum.melonds.domain.model.SaveStateSlot
import me.magnum.melonds.domain.repositories.SaveStatesRepository
import me.magnum.melonds.domain.repositories.SettingsRepository
import me.magnum.melonds.extensions.nameWithoutExtension
import me.magnum.melonds.ui.emulator.exceptions.SaveSlotLoadException
import java.util.*

class FileSystemSaveStatesRepository(
    private val settingsRepository: SettingsRepository,
    private val saveStateScreenshotProvider: SaveStateScreenshotProvider,
    private val uriHandler: UriHandler,
    private val context: Context
) : SaveStatesRepository {

    private companion object {
        const val LAST_MANUAL_SLOT = 8
    }

    override fun getRomSaveStates(rom: Rom): List<SaveStateSlot> {
        val existingSlots = getExistingSaveStateSlots(rom) ?: return emptyList()
        return (0..LAST_MANUAL_SLOT).map { existingSlots[it] ?: emptySlot(it) }
    }

    override fun getRomLoadableSaveStates(rom: Rom): List<SaveStateSlot> {
        val existingSlots = getExistingSaveStateSlots(rom) ?: return emptyList()
        val manualSlots = (1..LAST_MANUAL_SLOT).map { existingSlots[it] ?: emptySlot(it) }
        return sortedQuickSlots(existingSlots) + listOfNotNull(existingSlots[SaveStateSlot.AUTO_SAVE_SLOT]) + manualSlots
    }

    override fun getRomQuickSaveStateSlots(rom: Rom): List<SaveStateSlot> {
        return sortedQuickSlots(getExistingSaveStateSlots(rom).orEmpty())
    }

    override fun getRomNextQuickSaveStateSlot(rom: Rom): SaveStateSlot {
        val existingSlots = getExistingSaveStateSlots(rom).orEmpty()
        val extraSlots = settingsRepository.getRollingQuickSaveCount() - 1
        val ringSlots = listOf(SaveStateSlot.QUICK_SAVE_SLOT) + (SaveStateSlot.FIRST_ROLLING_SLOT until SaveStateSlot.FIRST_ROLLING_SLOT + extraSlots)

        val nextSlot = ringSlots.firstOrNull { it !in existingSlots }
            ?: ringSlots.minBy { existingSlots.getValue(it).lastUsedDate!! }
        return existingSlots[nextSlot] ?: emptySlot(nextSlot)
    }

    private fun sortedQuickSlots(existingSlots: Map<Int, SaveStateSlot>): List<SaveStateSlot> {
        return existingSlots.values
            .filter { it.isQuickSlot }
            .sortedByDescending { it.lastUsedDate }
            .ifEmpty { listOf(emptySlot(SaveStateSlot.QUICK_SAVE_SLOT)) }
    }

    private fun emptySlot(slot: Int) = SaveStateSlot(slot, false, null, null)

    private fun getExistingSaveStateSlots(rom: Rom): Map<Int, SaveStateSlot>? {
        val saveStateDirectoryDocument = getSaveStateDirectoryDocument(rom) ?: return null
        val romFileName = getRomFileNameWithoutExtension(rom) ?: return null

        val fileNameRegex = "${Regex.escape(romFileName)}\\.ml\\d+".toRegex()
        return listSaveStateFiles(saveStateDirectoryDocument) { it.matches(fileNameRegex) }.mapNotNull {
            val slotNumber = it.name.substringAfterLast(".ml").toIntOrNull() ?: return@mapNotNull null
            val slot = SaveStateSlot(slotNumber, true, Date(it.lastModified), null)
            val screenshotUri = saveStateScreenshotProvider.getRomSaveStateScreenshotUri(rom, slot)
            slotNumber to slot.copy(screenshot = screenshotUri)
        }.toMap()
    }

    override fun getRomSaveStateUri(rom: Rom, saveState: SaveStateSlot): Uri {
        val saveStateDirectoryDocument = getSaveStateDirectoryDocument(rom) ?: throw SaveSlotLoadException("Could not create parent directory document")

        val romFileName = getRomFileNameWithoutExtension(rom) ?: throw SaveSlotLoadException("Could not determine ROM file name")
        val saveStateName = "$romFileName.ml${saveState.slot}"
        val saveStateFile = listSaveStateFiles(saveStateDirectoryDocument) { it == saveStateName }.firstOrNull()

        val uri = if (saveStateFile != null) {
            saveStateFile.uri
        } else {
            saveStateDirectoryDocument.createFile("*/*", saveStateName)?.uri ?: throw SaveSlotLoadException("Could not create save state file")
        }

        return uri
    }

    override fun setRomSaveStateScreenshot(rom: Rom, saveState: SaveStateSlot, screenshot: Bitmap) {
        saveStateScreenshotProvider.saveRomSaveStateScreenshot(rom, saveState, screenshot)
    }

    override fun deleteRomSaveStateScreenshot(rom: Rom, saveState: SaveStateSlot) {
        saveStateScreenshotProvider.deleteRomSaveStateScreenshot(rom, saveState)
    }

    override fun deleteRomSaveState(rom: Rom, saveState: SaveStateSlot) {
        if (!saveState.exists) {
            return
        }

        val saveStateDirectoryDocument = getSaveStateDirectoryDocument(rom) ?: throw SaveSlotLoadException("Could not create parent directory document")
        val romFileName = getRomFileNameWithoutExtension(rom) ?: throw SaveSlotLoadException("Could not determine ROM file name")

        val saveStateName = "$romFileName.ml${saveState.slot}"
        val saveStateFile = saveStateDirectoryDocument.findFile(saveStateName)

        saveStateFile?.delete()
        saveStateScreenshotProvider.deleteRomSaveStateScreenshot(rom, saveState)
    }

    private data class SaveStateFile(val uri: Uri, val name: String, val lastModified: Long)

    private fun listSaveStateFiles(directory: DocumentFile, matches: (String) -> Boolean): List<SaveStateFile> {
        val directoryUri = directory.uri
        if (directoryUri.scheme == "content" && DocumentsContract.isTreeUri(directoryUri)) {
            val files = try {
                val childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(
                    directoryUri,
                    DocumentsContract.getDocumentId(directoryUri),
                )
                val projection = arrayOf(
                    DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                    DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                    DocumentsContract.Document.COLUMN_LAST_MODIFIED,
                )
                context.contentResolver.query(childrenUri, projection, null, null, null)?.use { cursor ->
                    buildList<SaveStateFile> {
                        while (cursor.moveToNext()) {
                            val name = cursor.getString(1)?.takeIf(matches) ?: continue
                            val documentId = cursor.getString(0) ?: continue
                            add(SaveStateFile(
                                DocumentsContract.buildDocumentUriUsingTree(directoryUri, documentId),
                                name,
                                if (cursor.isNull(2)) 0L else cursor.getLong(2),
                            ))
                        }
                    }
                }
            } catch (_: Exception) {
                null
            }
            if (files != null) {
                return files
            }
        }
        return directory.listFiles().mapNotNull {
            val name = it.name?.takeIf(matches) ?: return@mapNotNull null
            SaveStateFile(it.uri, name, it.lastModified())
        }
    }

    private fun getSaveStateDirectoryDocument(rom: Rom): DocumentFile? {
        val saveStateDirectoryUri = settingsRepository.getSaveStateDirectory(rom) ?: return null
        return uriHandler.getUriTreeDocument(saveStateDirectoryUri)
    }

    private fun getRomFileNameWithoutExtension(rom: Rom): String? {
        val romDocument = uriHandler.getUriDocument(rom.uri)
        return romDocument?.nameWithoutExtension
    }
}
