package me.magnum.melonds.impl.recording

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.media.MediaMuxer
import android.os.Bundle
import android.util.Log
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import me.magnum.melonds.MelonEmulator
import me.magnum.melonds.domain.model.CaptureScreens
import me.magnum.melonds.domain.model.rom.Rom
import me.magnum.melonds.domain.repositories.SettingsRepository
import java.io.File
import java.io.IOException
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.locks.LockSupport
import javax.inject.Inject
import kotlin.math.min

/**
 * Records the emulated screens and the game audio (not the device screen) into MP4 files.
 *
 * A single encoder pipeline feeds two consumers: a recording started with [startRecording], and a rolling clip buffer that keeps the last few seconds
 * in memory so [saveClip] can write them out. The pipeline only runs while one of them is in use.
 *
 * Timestamps follow emulated time ([MelonEmulator.getRecordingClockUs]) instead of real time, so recordings always play at normal speed: fast-forwarded
 * sections aren't sped up and pausing the game doesn't leave frozen sections in the video. The audio comes straight from the emulated sound chip, so it's
 * recorded at full volume even when the game is muted, turned down or fast-forwarded.
 */
class GameplayRecorder @Inject constructor(
    private val settingsRepository: SettingsRepository,
    private val recordingFileManager: RecordingFileManager,
) {
    companion object {
        private const val TAG = "GameplayRecorder"
        private const val SOURCE_WIDTH = 256
        private const val SOURCE_HEIGHT = 384
        private const val BITS_PER_PIXEL_PER_FRAME = 0.15
        // The clip buffer is kept in memory, so the bit rate is capped to keep long clips at high resolutions manageable
        private const val MAX_VIDEO_BIT_RATE = 8_000_000
        private const val AUDIO_BIT_RATE = 160_000
        private const val AUDIO_CHUNK_FRAMES = 1024
        private const val CODEC_TIMEOUT_US = 10_000L
        private const val IDLE_POLL_NS = 50_000_000L
        // Leaves room between captures for other emulator calls that share the capture lock (save states, pausing...)
        private const val MIN_CAPTURE_GAP_NS = 2_000_000L
        // While fast-forwarding, frames are captured at up to this many times the recording frame rate (in real time). Each capture costs the renderer
        // some time, so beyond this speed the fast-forwarded parts of the video get choppier instead of slowing the game down
        private const val MAX_CAPTURE_RATE_MULTIPLIER = 2
        private const val AUDIO_POLL_NS = 5_000_000L
        // Extra history kept so a clip can always start at a key frame (one is produced every second)
        private const val CLIP_KEY_FRAME_MARGIN_US = 2_000_000L
        private const val THREAD_JOIN_TIMEOUT_MS = 3_000L
    }

    class ClipUnavailableException : Exception("No gameplay has been buffered for a clip yet")
    class RecordingEmptyException : Exception("The recording didn't capture any frames")

    private enum class Track { VIDEO, AUDIO }

    private class EncodedSample(val data: ByteBuffer, val presentationTimeUs: Long, val flags: Int) {
        val isKeyFrame get() = (flags and MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0
    }

    private data class RecordingConfig(
        val requestedScale: Int,
        val screens: CaptureScreens,
        val width: Int,
        val height: Int,
        val frameRate: Int,
        val videoBitRate: Int,
        val sampleRate: Int,
        val clipLengthUs: Long,
    )

    /**
     * A recording that was stopped and still has to be written out with [saveRecording].
     */
    class PendingRecording internal constructor(internal val file: File, internal val rom: Rom) {
        private var writer: SampleWriter? = null
        private var error: Exception? = null

        internal fun write(videoFormat: MediaFormat?, audioFormat: MediaFormat?, isVideo: Boolean, data: ByteBuffer, presentationTimeUs: Long, flags: Int) {
            if (error != null) {
                return
            }
            try {
                val currentWriter = writer ?: run {
                    // The file starts at a key frame once both encoders have reported their output formats
                    if (!isVideo || (flags and MediaCodec.BUFFER_FLAG_KEY_FRAME) == 0 || videoFormat == null || audioFormat == null) {
                        return
                    }
                    SampleWriter(file, videoFormat, audioFormat, presentationTimeUs).also { writer = it }
                }
                currentWriter.write(isVideo, data, presentationTimeUs, flags)
            } catch (exception: Exception) {
                Log.w(TAG, "Failed to write recording", exception)
                error = exception
            }
        }

        internal fun finish(): Boolean {
            val finished = writer?.finish() ?: false
            error?.let { throw it }
            return finished
        }
    }

    internal class SampleWriter(file: File, videoFormat: MediaFormat, audioFormat: MediaFormat, private val basePresentationTimeUs: Long) {
        private val muxer = MediaMuxer(file.path, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4)
        private val videoTrack: Int
        private val audioTrack: Int
        private val info = MediaCodec.BufferInfo()
        private var lastAudioPresentationTimeUs = -1L
        private var videoSamples = 0

        init {
            val tracks = try {
                val tracks = muxer.addTrack(videoFormat) to muxer.addTrack(audioFormat)
                muxer.start()
                tracks
            } catch (exception: Exception) {
                muxer.release()
                throw exception
            }
            videoTrack = tracks.first
            audioTrack = tracks.second
        }

        fun write(isVideo: Boolean, data: ByteBuffer, presentationTimeUs: Long, flags: Int) {
            val relativeTimeUs = presentationTimeUs - basePresentationTimeUs
            if (relativeTimeUs < 0) {
                return
            }
            if (!isVideo) {
                // Audio timestamps must keep increasing
                if (relativeTimeUs <= lastAudioPresentationTimeUs) {
                    return
                }
                lastAudioPresentationTimeUs = relativeTimeUs
            }
            val buffer = data.duplicate().apply { rewind() }
            info.set(0, buffer.remaining(), relativeTimeUs, flags and MediaCodec.BUFFER_FLAG_KEY_FRAME)
            muxer.writeSampleData(if (isVideo) videoTrack else audioTrack, buffer, info)
            if (isVideo) {
                videoSamples++
            }
        }

        /**
         * Finalises the file. Returns false if no frames were written, in which case the file is not playable.
         */
        fun finish(): Boolean {
            try {
                if (videoSamples > 0) {
                    muxer.stop()
                }
            } finally {
                muxer.release()
            }
            return videoSamples > 0
        }
    }

    // Serialises the public operations, which may block while the encoders shut down. Never taken by the encoder threads
    private val operationLock = Any()
    // Guards the state shared with the encoder threads
    private val lock = Any()

    private var pipeline: EncoderPipeline? = null
    private var clipBufferEnabled = false
    private var activeRecording: PendingRecording? = null
    private var released = false
    private val clipVideo = ArrayDeque<EncodedSample>()
    private val clipAudio = ArrayDeque<EncodedSample>()

    private val _isRecording = MutableStateFlow(false)
    val isRecording: StateFlow<Boolean> = _isRecording.asStateFlow()

    /**
     * Keeps the last few seconds of gameplay in memory so they can be saved with [saveClip]. Blocks while the encoders start or stop.
     */
    fun setClipBufferEnabled(enabled: Boolean) {
        synchronized(operationLock) {
            var pipelineToStop: EncoderPipeline? = null
            synchronized(lock) {
                if (released || clipBufferEnabled == enabled) {
                    return
                }
                clipBufferEnabled = enabled
                if (!enabled) {
                    clipVideo.clear()
                    clipAudio.clear()
                    pipelineToStop = detachPipelineIfUnused()
                }
            }
            pipelineToStop?.stop()
            if (enabled) {
                ensurePipeline()
            }
        }
    }

    /**
     * Starts recording [rom]'s gameplay. Blocks while the encoders start.
     */
    fun startRecording(rom: Rom) {
        synchronized(operationLock) {
            if (released || activeRecording != null) {
                return
            }
            // The clip buffer may still be running with older settings
            restartPipelineIfOutdated()
            val currentPipeline = ensurePipeline() ?: throw IOException("Video encoder unavailable")
            val file = recordingFileManager.createTempFile()
            synchronized(lock) {
                activeRecording = PendingRecording(file, rom)
                _isRecording.value = true
            }
            currentPipeline.requestKeyFrame()
        }
    }

    /**
     * Restarts the encoders if the recording settings changed. Does nothing while a recording is in progress. Blocks while the encoders restart.
     */
    fun applySettings() {
        synchronized(operationLock) {
            restartPipelineIfOutdated()
        }
    }

    /**
     * Stops the current recording. The returned recording must be written out with [saveRecording].
     */
    fun stopRecording(): PendingRecording? {
        synchronized(operationLock) {
            var pipelineToStop: EncoderPipeline? = null
            var recording: PendingRecording? = null
            synchronized(lock) {
                recording = activeRecording
                activeRecording = null
                _isRecording.value = false
                pipelineToStop = detachPipelineIfUnused()
            }
            pipelineToStop?.stop()
            return recording
        }
    }

    /**
     * Ends the recording and discards the clip buffer of the game that just stopped. The encoders are restarted (with the current settings) if the clip
     * buffer is still in use.
     */
    fun onEmulatorStopped(): PendingRecording? {
        synchronized(operationLock) {
            val recording = detachEverything()
            if (clipBufferEnabled) {
                ensurePipeline()
            }
            return recording
        }
    }

    /**
     * Stops everything for good. A recording in progress is returned so it can still be saved.
     */
    fun release(): PendingRecording? {
        synchronized(operationLock) {
            val recording = detachEverything()
            synchronized(lock) {
                released = true
                clipBufferEnabled = false
            }
            return recording
        }
    }

    /**
     * Finalises a stopped recording and copies it to the recordings folder. Returns the saved file name.
     */
    fun saveRecording(recording: PendingRecording): String {
        val hasFrames = try {
            recording.finish()
        } catch (exception: Exception) {
            recordingFileManager.discardTempFile(recording.file)
            throw exception
        }
        if (!hasFrames) {
            recordingFileManager.discardTempFile(recording.file)
            throw RecordingEmptyException()
        }
        return recordingFileManager.save(recording.rom, recording.file, "Recording")
    }

    /**
     * Writes the buffered gameplay (up to the configured clip length) to the recordings folder. Returns the saved file name.
     */
    fun saveClip(rom: Rom): String {
        val snapshot = synchronized(lock) {
            pipeline?.takeIf { clipBufferEnabled }?.let { Triple(it, clipVideo.toList(), clipAudio.toList()) }
        } ?: throw ClipUnavailableException()
        val (currentPipeline, videoSamples, audioSamples) = snapshot
        val videoFormat = currentPipeline.videoFormat
        val audioFormat = currentPipeline.audioFormat
        val clipLengthUs = currentPipeline.config.clipLengthUs
        if (videoSamples.isEmpty() || videoFormat == null || audioFormat == null) {
            throw ClipUnavailableException()
        }

        val clipStartUs = videoSamples.maxOf { it.presentationTimeUs } - clipLengthUs
        // Start at the last key frame before the requested start, or at the first one if less than a full clip has been buffered
        val startIndex = videoSamples.indexOfLast { it.isKeyFrame && it.presentationTimeUs <= clipStartUs }.takeIf { it >= 0 }
            ?: videoSamples.indexOfFirst { it.isKeyFrame }
        if (startIndex < 0) {
            throw ClipUnavailableException()
        }

        val tempFile = recordingFileManager.createTempFile()
        val hasFrames = try {
            val writer = SampleWriter(tempFile, videoFormat, audioFormat, videoSamples[startIndex].presentationTimeUs)
            try {
                // Interleave both tracks by time, which is how the muxer expects to receive them
                var videoIndex = startIndex
                var audioIndex = 0
                while (videoIndex < videoSamples.size || audioIndex < audioSamples.size) {
                    val video = videoSamples.getOrNull(videoIndex)
                    val audio = audioSamples.getOrNull(audioIndex)
                    if (audio == null || (video != null && video.presentationTimeUs <= audio.presentationTimeUs)) {
                        writer.write(true, video!!.data, video.presentationTimeUs, video.flags)
                        videoIndex++
                    } else {
                        writer.write(false, audio.data, audio.presentationTimeUs, audio.flags)
                        audioIndex++
                    }
                }
            } catch (exception: Exception) {
                runCatching { writer.finish() }
                throw exception
            }
            writer.finish()
        } catch (exception: Exception) {
            recordingFileManager.discardTempFile(tempFile)
            throw exception
        }
        if (!hasFrames) {
            recordingFileManager.discardTempFile(tempFile)
            throw ClipUnavailableException()
        }
        return recordingFileManager.save(rom, tempFile, "Clip")
    }

    private fun detachEverything(): PendingRecording? {
        var pipelineToStop: EncoderPipeline? = null
        var recording: PendingRecording? = null
        synchronized(lock) {
            recording = activeRecording
            activeRecording = null
            _isRecording.value = false
            clipVideo.clear()
            clipAudio.clear()
            pipelineToStop = pipeline
            pipeline = null
        }
        pipelineToStop?.stop()
        return recording
    }

    /**
     * Must be called with [lock] held. The returned pipeline must be stopped after releasing the lock, since its threads need it to finish.
     */
    private fun detachPipelineIfUnused(): EncoderPipeline? {
        if (clipBufferEnabled || activeRecording != null) {
            return null
        }
        return pipeline.also { pipeline = null }
    }

    /**
     * Must be called with [operationLock] held. The clip buffer is dropped, since its samples can't be mixed with the output of new encoders.
     */
    private fun restartPipelineIfOutdated() {
        var pipelineToStop: EncoderPipeline? = null
        synchronized(lock) {
            val currentPipeline = pipeline ?: return
            if (activeRecording != null || !isOutdated(currentPipeline.config)) {
                return
            }
            clipVideo.clear()
            clipAudio.clear()
            pipelineToStop = currentPipeline
            pipeline = null
        }
        pipelineToStop?.stop()
        if (clipBufferEnabled) {
            ensurePipeline()
        }
    }

    private fun isOutdated(config: RecordingConfig): Boolean {
        return config.requestedScale != settingsRepository.getRecordingScale() ||
            config.screens != settingsRepository.getRecordingScreens() ||
            config.frameRate != settingsRepository.getRecordingFrameRate() ||
            config.clipLengthUs != settingsRepository.getClipLengthSeconds() * 1_000_000L
    }

    /**
     * Must be called with [operationLock] held.
     */
    private fun ensurePipeline(): EncoderPipeline? {
        synchronized(lock) {
            if (released) {
                return null
            }
            pipeline?.let { return it }
        }

        val frameRate = settingsRepository.getRecordingFrameRate()
        val clipLengthUs = settingsRepository.getClipLengthSeconds() * 1_000_000L
        val sampleRate = MelonEmulator.getRecordingAudioSampleRate()
        val requestedScale = settingsRepository.getRecordingScale()
        val screens = settingsRepository.getRecordingScreens()
        // Large sizes aren't supported by every encoder, so fall back to smaller ones
        for (scale in requestedScale downTo 1) {
            val width = SOURCE_WIDTH * scale
            val height = screens.sourceHeight * scale
            val config = RecordingConfig(
                requestedScale = requestedScale,
                screens = screens,
                width = width,
                height = height,
                frameRate = frameRate,
                videoBitRate = (width * height * frameRate * BITS_PER_PIXEL_PER_FRAME).toInt().coerceIn(1_000_000, MAX_VIDEO_BIT_RATE),
                sampleRate = sampleRate,
                clipLengthUs = clipLengthUs,
            )
            val newPipeline = try {
                EncoderPipeline(config)
            } catch (exception: Exception) {
                Log.w(TAG, "Cannot start the encoders at ${width}x$height", exception)
                continue
            }
            synchronized(lock) {
                pipeline = newPipeline
            }
            newPipeline.start()
            return newPipeline
        }
        return null
    }

    private fun onEncodedSample(source: EncoderPipeline, track: Track, sample: EncodedSample) {
        synchronized(lock) {
            if (source !== pipeline) {
                return
            }
            if (clipBufferEnabled) {
                if (track == Track.VIDEO) clipVideo.addLast(sample) else clipAudio.addLast(sample)
                val newestTimeUs = clipVideo.lastOrNull()?.presentationTimeUs
                if (newestTimeUs != null) {
                    val cutoffUs = newestTimeUs - source.config.clipLengthUs - CLIP_KEY_FRAME_MARGIN_US
                    while (clipVideo.isNotEmpty() && clipVideo.first().presentationTimeUs < cutoffUs) clipVideo.removeFirst()
                    while (clipAudio.isNotEmpty() && clipAudio.first().presentationTimeUs < cutoffUs) clipAudio.removeFirst()
                }
            }
            activeRecording?.write(source.videoFormat, source.audioFormat, track == Track.VIDEO, sample.data, sample.presentationTimeUs, sample.flags)
        }
    }

    private inner class EncoderPipeline(val config: RecordingConfig) {
        private val videoCodec: MediaCodec
        private val audioCodec: MediaCodec
        private val frameBuffer = ByteBuffer.allocateDirect(SOURCE_WIDTH * SOURCE_HEIGHT * 4).order(ByteOrder.nativeOrder())
        private val videoThread = Thread({ runSafely("video", ::runVideo) }, "GameplayRecorder-Video")
        private val audioThread = Thread({ runSafely("audio", ::runAudio) }, "GameplayRecorder-Audio")

        @Volatile private var running = true
        @Volatile var videoFormat: MediaFormat? = null
            private set
        @Volatile var audioFormat: MediaFormat? = null
            private set

        // Emulated time and dropped audio when the pipeline started. Both tracks are timed from this point
        private var baseClockUs = 0L
        private var baseDroppedAudioFrames = 0L

        init {
            val videoEncoderFormat = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, config.width, config.height).apply {
                setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible)
                setInteger(MediaFormat.KEY_BIT_RATE, config.videoBitRate)
                setInteger(MediaFormat.KEY_FRAME_RATE, config.frameRate)
                setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
            }
            val audioEncoderFormat = MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_AAC, config.sampleRate, 2).apply {
                setInteger(MediaFormat.KEY_AAC_PROFILE, MediaCodecInfo.CodecProfileLevel.AACObjectLC)
                setInteger(MediaFormat.KEY_BIT_RATE, AUDIO_BIT_RATE)
                setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, AUDIO_CHUNK_FRAMES * 4 * 4)
            }

            val createdCodecs = mutableListOf<MediaCodec>()
            val codecs = try {
                val video = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC).also { createdCodecs.add(it) }
                video.configure(videoEncoderFormat, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
                val audio = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_AUDIO_AAC).also { createdCodecs.add(it) }
                audio.configure(audioEncoderFormat, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
                video.start()
                audio.start()
                video to audio
            } catch (exception: Exception) {
                createdCodecs.forEach { runCatching { it.release() } }
                throw exception
            }
            videoCodec = codecs.first
            audioCodec = codecs.second
        }

        fun start() {
            baseDroppedAudioFrames = MelonEmulator.getRecordingAudioDroppedFrames()
            baseClockUs = MelonEmulator.getRecordingClockUs()
            // Audio is captured from here on, so its first sample lines up with baseClockUs
            MelonEmulator.setRecordingAudioCaptureEnabled(true)
            videoThread.start()
            audioThread.start()
        }

        fun stop() {
            running = false
            videoThread.join(THREAD_JOIN_TIMEOUT_MS)
            audioThread.join(THREAD_JOIN_TIMEOUT_MS)
            MelonEmulator.setRecordingAudioCaptureEnabled(false)
            listOf(videoCodec, audioCodec).forEach {
                runCatching { it.stop() }
                runCatching { it.release() }
            }
        }

        fun requestKeyFrame() {
            runCatching {
                videoCodec.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0) })
            }
        }

        private fun runSafely(name: String, block: () -> Unit) {
            try {
                block()
            } catch (exception: Exception) {
                if (running) {
                    Log.e(TAG, "The $name encoder stopped", exception)
                }
            }
        }

        private fun clockUs(): Long {
            return MelonEmulator.getRecordingClockUs() - baseClockUs
        }

        private fun runVideo() {
            val frameIntervalUs = 1_000_000L / config.frameRate
            val minCaptureIntervalNs = 1_000_000_000L / (config.frameRate * MAX_CAPTURE_RATE_MULTIPLIER)
            val info = MediaCodec.BufferInfo()
            // Frames sit on a fixed grid of emulated time, one slot per frame
            var nextFrameIndex = 0L
            var nextCaptureNs = System.nanoTime()
            while (running) {
                val waitNs = maxOf((nextFrameIndex * frameIntervalUs - clockUs()) * 1000, nextCaptureNs - System.nanoTime())
                if (waitNs > 0) {
                    // Emulated time stands still while the game is paused, so the wait is capped to keep checking for it
                    drain(videoCodec, Track.VIDEO, info)
                    LockSupport.parkNanos(min(waitNs, IDLE_POLL_NS))
                    continue
                }

                if (!MelonEmulator.captureRecordingFrame(frameBuffer)) {
                    // The emulator is paused or not running
                    nextCaptureNs = System.nanoTime() + IDLE_POLL_NS
                    drain(videoCodec, Track.VIDEO, info)
                    continue
                }

                val captureNs = System.nanoTime()
                // While fast-forwarding, several slots may have passed since the last capture. Skipping them keeps the video at normal speed
                val frameIndex = maxOf(nextFrameIndex, clockUs() / frameIntervalUs)
                nextFrameIndex = frameIndex + 1
                nextCaptureNs = captureNs + maxOf(minCaptureIntervalNs, MIN_CAPTURE_GAP_NS)

                encodeFrame(frameIndex * frameIntervalUs)
                drain(videoCodec, Track.VIDEO, info)
            }
        }

        private fun encodeFrame(presentationTimeUs: Long) {
            val index = videoCodec.dequeueInputBuffer(CODEC_TIMEOUT_US)
            if (index < 0) {
                // The encoder is busy. Drop this frame
                return
            }
            val image = videoCodec.getInputImage(index)
            val planes = image?.planes
            val converted = planes != null && planes.size >= 3 && planes.all { it.buffer.isDirect } && MelonEmulator.convertRecordingFrame(
                frameBuffer,
                config.screens.sourceY,
                config.screens.sourceHeight,
                config.width,
                config.height,
                planes[0].buffer, planes[0].rowStride, planes[0].pixelStride,
                planes[1].buffer, planes[1].rowStride, planes[1].pixelStride,
                planes[2].buffer, planes[2].rowStride, planes[2].pixelStride,
            )
            val size = if (converted) config.width * config.height * 3 / 2 else 0
            videoCodec.queueInputBuffer(index, 0, size, presentationTimeUs, 0)
        }

        private fun runAudio() {
            val pcm = ByteBuffer.allocateDirect(AUDIO_CHUNK_FRAMES * 4).order(ByteOrder.nativeOrder())
            val info = MediaCodec.BufferInfo()
            var queuedFrames = 0L
            var inputIndex = -1
            while (running) {
                if (inputIndex < 0) {
                    inputIndex = audioCodec.dequeueInputBuffer(CODEC_TIMEOUT_US)
                    if (inputIndex < 0) {
                        drain(audioCodec, Track.AUDIO, info)
                        continue
                    }
                }
                val input = audioCodec.getInputBuffer(inputIndex) ?: break
                val maxFrames = min(AUDIO_CHUNK_FRAMES, input.capacity() / 4)

                val frames = MelonEmulator.readRecordingAudio(pcm, maxFrames)
                if (frames <= 0) {
                    drain(audioCodec, Track.AUDIO, info)
                    LockSupport.parkNanos(AUDIO_POLL_NS)
                    continue
                }

                // The audio follows emulated time, so its timestamp comes from its position in the stream. Audio that was dropped because this thread
                // fell behind still took up time, so it's counted to stay in sync with the video
                val droppedFrames = MelonEmulator.getRecordingAudioDroppedFrames() - baseDroppedAudioFrames
                val presentationTimeUs = (queuedFrames + droppedFrames) * 1_000_000L / config.sampleRate
                pcm.position(0).limit(frames * 4)
                input.clear()
                input.put(pcm)
                audioCodec.queueInputBuffer(inputIndex, 0, frames * 4, presentationTimeUs, 0)
                inputIndex = -1
                queuedFrames += frames
                drain(audioCodec, Track.AUDIO, info)
            }
        }

        private fun drain(codec: MediaCodec, track: Track, info: MediaCodec.BufferInfo) {
            while (true) {
                val index = codec.dequeueOutputBuffer(info, 0)
                when {
                    index == MediaCodec.INFO_TRY_AGAIN_LATER -> return
                    index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                        if (track == Track.VIDEO) videoFormat = codec.outputFormat else audioFormat = codec.outputFormat
                    }
                    index >= 0 -> {
                        val output = codec.getOutputBuffer(index)
                        if (output != null && info.size > 0 && (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) == 0) {
                            output.limit(info.offset + info.size)
                            output.position(info.offset)
                            val data = ByteBuffer.allocateDirect(info.size)
                            data.put(output)
                            data.flip()
                            onEncodedSample(this, track, EncodedSample(data, info.presentationTimeUs, info.flags))
                        }
                        codec.releaseOutputBuffer(index, false)
                    }
                }
            }
        }
    }
}
