package me.magnum.melonds.impl.recording

import android.content.Context
import androidx.documentfile.provider.DocumentFile
import dagger.hilt.android.qualifiers.ApplicationContext
import me.magnum.melonds.common.uridelegates.UriHandler
import me.magnum.melonds.domain.model.rom.Rom
import me.magnum.melonds.domain.repositories.SettingsRepository
import java.io.File
import java.io.IOException
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Recordings and clips are muxed into a temporary file first and copied to the recordings folder once they're complete. Temporary files left behind
 * (the app was killed mid-recording, a save failed...) can be removed with [deleteLeftoverFiles].
 */
@Singleton
class RecordingFileManager @Inject constructor(
    @param:ApplicationContext private val context: Context,
    private val settingsRepository: SettingsRepository,
    private val uriHandler: UriHandler,
) {
    companion object {
        private const val TEMP_DIRECTORY = "recordings_temp"
        private const val TEMP_EXTENSION = ".mp4.tmp"
    }

    data class LeftoverFiles(val count: Int, val totalBytes: Long)

    private val activeTempFiles = mutableSetOf<File>()

    fun createTempFile(): File = synchronized(activeTempFiles) {
        val directory = getTempDirectory()
        if (!directory.isDirectory && !directory.mkdirs()) {
            throw IOException("Cannot create temporary recordings directory")
        }
        val timestamp = SimpleDateFormat("yyyy-MM-dd_HH-mm-ss-SSS", Locale.ROOT).format(Date())
        var file = File(directory, "recording-$timestamp$TEMP_EXTENSION")
        var suffix = 1
        while (file.exists() || activeTempFiles.contains(file)) {
            file = File(directory, "recording-$timestamp-${suffix++}$TEMP_EXTENSION")
        }
        activeTempFiles.add(file)
        file
    }

    fun discardTempFile(file: File) {
        synchronized(activeTempFiles) {
            activeTempFiles.remove(file)
        }
        file.delete()
    }

    /**
     * Copies a finished temporary recording into the recordings folder and deletes the temporary file. Returns the saved file name.
     */
    fun save(rom: Rom, tempFile: File, kind: String): String {
        try {
            val directoryUri = settingsRepository.getRecordingDirectory()
            val directory = if (directoryUri != null) {
                uriHandler.getUriTreeDocument(directoryUri)
            } else {
                val folder = File(context.getExternalFilesDir(null) ?: context.filesDir, "recordings")
                if (!folder.isDirectory && !folder.mkdirs()) {
                    throw IOException("Cannot create recordings directory")
                }
                DocumentFile.fromFile(folder)
            } ?: throw IOException("Recordings directory unavailable")
            if (!directory.isDirectory || !directory.canWrite()) {
                throw IOException("Recordings directory is not writable")
            }

            val romName = rom.fileName.substringBeforeLast('.').replace(Regex("[\\\\/:*?\"<>|\\p{Cntrl}]"), "_")
                .trim().trim('.').take(120).ifEmpty { "ROM" }
            val timestamp = SimpleDateFormat("yyyy-MM-dd_HH-mm-ss", Locale.ROOT).format(Date())
            val name = "$romName - $kind $timestamp.mp4"
            if (directory.findFile(name) != null) {
                throw IOException("Recording already exists")
            }
            // File-backed documents append the extension from the MIME type themselves
            val documentName = if (directory.uri.scheme == "file") name.removeSuffix(".mp4") else name
            val document = directory.createFile("video/mp4", documentName)
                ?: throw IOException("Cannot create recording")
            try {
                val output = context.contentResolver.openOutputStream(document.uri, "w")
                    ?: throw IOException("Cannot open recording output")
                output.use { stream ->
                    tempFile.inputStream().use { it.copyTo(stream) }
                }
            } catch (exception: Exception) {
                document.delete()
                throw exception
            }
            return document.name ?: name
        } finally {
            discardTempFile(tempFile)
        }
    }

    /**
     * Temporary recordings that aren't being written to right now.
     */
    fun getLeftoverFiles(): LeftoverFiles {
        val files = listLeftoverFiles()
        return LeftoverFiles(files.size, files.sumOf { it.length() })
    }

    fun deleteLeftoverFiles(): LeftoverFiles {
        var count = 0
        var totalBytes = 0L
        listLeftoverFiles().forEach {
            val size = it.length()
            if (it.delete()) {
                count++
                totalBytes += size
            }
        }
        return LeftoverFiles(count, totalBytes)
    }

    private fun listLeftoverFiles(): List<File> {
        val active = synchronized(activeTempFiles) { activeTempFiles.toSet() }
        return getTempDirectory().listFiles()?.filter { it.isFile && it !in active }.orEmpty()
    }

    private fun getTempDirectory(): File {
        return File(context.getExternalFilesDir(null) ?: context.filesDir, TEMP_DIRECTORY)
    }
}
