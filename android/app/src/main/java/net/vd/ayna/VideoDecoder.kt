package net.vd.ayna

import android.media.MediaCodec
import android.media.MediaFormat
import android.os.Build
import android.os.Process
import android.util.Log
import android.view.Surface
import java.nio.ByteBuffer

/**
 * Donanım H.264 çözücü. Kareler geldiği anda çözülür ve beklemeden ekrana verilir
 * (tampon yok, vsync beklenmez).
 */
class VideoDecoder(private val surface: Surface) {

    private class ParamSets(val sps: ByteArray, val pps: ByteArray)

    private var codec: MediaCodec? = null
    private var outThread: Thread? = null
    @Volatile private var outRunning = false

    private var curW = 0
    private var curH = 0
    private var curSps: ByteArray? = null

    private var pts = 0L
    private val t0s = LongArray(RING)

    /** İlk paketin alınmasından kare ekrana verilene kadar geçen süre (ns). */
    var onRendered: ((Long) -> Unit)? = null

    /** Çağıran tek bir iş parçacığıdır (alım iş parçacığı). */
    fun decode(data: ByteArray, len: Int, key: Boolean, w: Int, h: Int, t0: Long): Boolean {
        if (key) {
            val ps = findParamSets(data, len)
            if (ps == null) {
                if (codec == null) return false
            } else if (codec == null || w != curW || h != curH || !ps.sps.contentEquals(curSps)) {
                release()
                if (!create(w, h, ps)) return false
            }
        }
        val c = codec ?: return false
        return try {
            val idx = c.dequeueInputBuffer(15_000)
            if (idx < 0) return false
            val ib = c.getInputBuffer(idx) ?: return false
            ib.clear()
            if (len > ib.capacity()) {
                c.queueInputBuffer(idx, 0, 0, 0, 0)
                return false
            }
            ib.put(data, 0, len)
            val p = pts++
            t0s[(p % RING).toInt()] = t0
            c.queueInputBuffer(idx, 0, len, p, if (key) MediaCodec.BUFFER_FLAG_KEY_FRAME else 0)
            true
        } catch (e: Exception) {
            Log.w(TAG, "decode hatası, çözücü sıfırlanıyor", e)
            release()
            false
        }
    }

    private fun create(w: Int, h: Int, ps: ParamSets): Boolean {
        val mime = MediaFormat.MIMETYPE_VIDEO_AVC
        val c = try {
            MediaCodec.createDecoderByType(mime)
        } catch (e: Exception) {
            Log.e(TAG, "çözücü bulunamadı", e)
            return false
        }

        fun format(lowLatencyExtras: Boolean) = MediaFormat.createVideoFormat(mime, w, h).apply {
            setByteBuffer("csd-0", ByteBuffer.wrap(START + ps.sps))
            setByteBuffer("csd-1", ByteBuffer.wrap(START + ps.pps))
            setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, maxOf(w * h * 3 / 2, 1 shl 20))
            setInteger(MediaFormat.KEY_COLOR_STANDARD, MediaFormat.COLOR_STANDARD_BT709)
            setInteger(MediaFormat.KEY_COLOR_RANGE, MediaFormat.COLOR_RANGE_LIMITED)
            setInteger(MediaFormat.KEY_COLOR_TRANSFER, MediaFormat.COLOR_TRANSFER_SDR_VIDEO)
            if (lowLatencyExtras) {
                if (Build.VERSION.SDK_INT >= 30) setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
                setInteger(MediaFormat.KEY_PRIORITY, 0)
                // Üreticiye özel düşük gecikme anahtarları; bilmeyen çözücü yok sayar.
                setInteger("vendor.qti-ext-dec-low-latency.enable", 1)
                setInteger("vendor.qti-ext-dec-picture-order.enable", 1)
                setInteger("vendor.low-latency.enable", 1)
                setInteger("vendor.rtc-ext-dec-low-latency.enable", 1)
            }
        }

        try {
            try {
                c.configure(format(true), surface, null, 0)
            } catch (e: Exception) {
                Log.w(TAG, "düşük gecikme ayarları reddedildi, sade yapılandırma deneniyor", e)
                c.reset()
                c.configure(format(false), surface, null, 0)
            }
            c.setVideoScalingMode(MediaCodec.VIDEO_SCALING_MODE_SCALE_TO_FIT)
            c.start()
        } catch (e: Exception) {
            Log.e(TAG, "çözücü başlatılamadı", e)
            try { c.release() } catch (_: Exception) {}
            return false
        }

        codec = c
        curW = w
        curH = h
        curSps = ps.sps
        outRunning = true
        outThread = Thread({ outputLoop(c) }, "ayna-dec-out").also { it.start() }
        Log.i(TAG, "çözücü hazır: ${c.name} ${w}x$h")
        return true
    }

    private fun outputLoop(c: MediaCodec) {
        Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_DISPLAY)
        val info = MediaCodec.BufferInfo()
        while (outRunning) {
            val idx = try {
                c.dequeueOutputBuffer(info, 10_000)
            } catch (e: Exception) {
                break
            }
            if (idx >= 0) {
                try {
                    c.releaseOutputBuffer(idx, true)
                } catch (e: Exception) {
                    break
                }
                val t0 = t0s[(info.presentationTimeUs % RING).toInt()]
                onRendered?.invoke(System.nanoTime() - t0)
            }
        }
    }

    fun release() {
        outRunning = false
        outThread?.let {
            try { it.join(500) } catch (_: InterruptedException) {}
        }
        outThread = null
        codec?.let {
            try { it.stop() } catch (_: Exception) {}
            try { it.release() } catch (_: Exception) {}
        }
        codec = null
        curSps = null
    }

    /** Annex B akışında ilk SPS (7) ve PPS (8) NAL birimlerini bulur (başlangıç kodu olmadan). */
    private fun findParamSets(d: ByteArray, len: Int): ParamSets? {
        var sps: ByteArray? = null
        var pps: ByteArray? = null
        var nalStart = -1

        fun take(start: Int, endExclusive: Int) {
            if (endExclusive <= start) return
            when (d[start].toInt() and 0x1F) {
                7 -> if (sps == null) sps = d.copyOfRange(start, endExclusive)
                8 -> if (pps == null) pps = d.copyOfRange(start, endExclusive)
            }
        }

        var i = 0
        while (i + 3 <= len) {
            if (d[i].toInt() == 0 && d[i + 1].toInt() == 0 && d[i + 2].toInt() == 1) {
                if (nalStart >= 0) {
                    var end = i
                    if (end > nalStart && d[end - 1].toInt() == 0) end-- // 4 baytlık başlangıç kodu
                    take(nalStart, end)
                    if (sps != null && pps != null) break
                }
                i += 3
                nalStart = i
            } else {
                i++
            }
        }
        if (nalStart >= 0 && (sps == null || pps == null)) take(nalStart, len)

        val s = sps ?: return null
        val p = pps ?: return null
        return ParamSets(s, p)
    }

    companion object {
        private const val TAG = "AynaDecoder"
        private const val RING = 64
        private val START = byteArrayOf(0, 0, 0, 1)
    }
}
