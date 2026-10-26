package net.vd.yansi

import android.os.Process
import android.os.SystemClock
import android.view.Surface
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.NetworkInterface
import java.net.SocketTimeoutException
import java.util.Collections
import java.util.Locale
import java.util.concurrent.atomic.AtomicLong

/**
 * UDP alıcısı: paketleri karelere birleştirir, kayıpta IDR ister,
 * keepalive/ping gönderir ve keşif sinyali yayınlar.
 */
class StreamClient(
    surface: Surface,
    private val deviceName: String,
    private val listener: Listener,
) {
    interface Listener {
        fun onSenderChanged(sender: String?)
        fun onVideoSize(width: Int, height: Int)
        fun onStats(text: String)
    }

    private class Slot(
        val id: Int,
        val count: Int,
        val key: Boolean,
        val width: Int,
        val height: Int,
        val t0: Long,
    ) {
        val data = ByteArray(count * Proto.PAYLOAD)
        val got = BooleanArray(count)
        var received = 0
        var lastLen = 0

        // FEC: her FEC_GROUP veri paketine bir XOR eşlik paketi
        val groups = (count + Proto.FEC_GROUP - 1) / Proto.FEC_GROUP
        val parity = arrayOfNulls<ByteArray>(groups)
        val parityLen = IntArray(groups)
        val groupGot = IntArray(groups)
    }

    private val decoder = VideoDecoder(surface)
    @Volatile private var running = false
    private var socket: DatagramSocket? = null
    private val threads = ArrayList<Thread>()

    @Volatile private var sender: InetSocketAddress? = null
    @Volatile private var lastSeenMs = 0L

    // Yalnızca alım iş parçacığının dokunduğu durum
    private val slots = ArrayList<Slot>(MAX_SLOTS + 1)
    private var lastId = 0
    private var haveLast = false
    private var waitingKey = true
    private var lastIdrMs = 0L
    private var videoW = 0
    private var videoH = 0

    // İstatistik
    private val bytes = AtomicLong()
    private val frames = AtomicLong()
    private val drops = AtomicLong()
    private val idrRequests = AtomicLong()
    private val recovered = AtomicLong()
    private val latSumNs = AtomicLong()
    private val latCount = AtomicLong()
    @Volatile private var rttNs = -1L

    fun start() {
        socket = DatagramSocket(null).apply {
            reuseAddress = true
            receiveBufferSize = 8 * 1024 * 1024
            bind(InetSocketAddress(Proto.VIDEO_PORT))
            soTimeout = 250
            trafficClass = 0xB8
        }
        decoder.onRendered = { ns ->
            latSumNs.addAndGet(ns)
            latCount.incrementAndGet()
            frames.incrementAndGet()
        }
        running = true
        spawn("yansi-rx", Process.THREAD_PRIORITY_URGENT_DISPLAY) { rxLoop() }
        spawn("yansi-ctl", Process.THREAD_PRIORITY_DEFAULT) { controlLoop() }
        spawn("yansi-beacon", Process.THREAD_PRIORITY_BACKGROUND) { beaconLoop() }
    }

    fun stop() {
        running = false
        try { socket?.close() } catch (_: Exception) {}
        for (t in threads) {
            t.interrupt()
            try { t.join(1000) } catch (_: InterruptedException) {}
        }
        threads.clear()
        decoder.release()
    }

    private fun spawn(name: String, priority: Int, body: () -> Unit) {
        val t = Thread({
            Process.setThreadPriority(priority)
            body()
        }, name)
        t.start()
        threads.add(t)
    }

    // ---------------------------------------------------------------- alım

    private fun rxLoop() {
        val sock = socket ?: return
        val buf = ByteArray(2048)
        val pkt = DatagramPacket(buf, buf.size)
        while (running) {
            pkt.length = buf.size
            try {
                sock.receive(pkt)
            } catch (_: SocketTimeoutException) {
                continue
            } catch (_: Exception) {
                if (!running) break
                continue
            }
            val len = pkt.length
            if (len < 4 || buf[0] != Proto.MAGIC0 || buf[1] != Proto.MAGIC1) continue
            val from = pkt.socketAddress as? InetSocketAddress ?: continue
            when (buf[2].toInt() and 0xFF) {
                Proto.T_VIDEO -> onVideo(buf, len, from, false)
                Proto.T_FEC -> onVideo(buf, len, from, true)
                Proto.T_PONG -> if (len >= 12 && from == sender) {
                    lastSeenMs = SystemClock.elapsedRealtime()
                    rttNs = System.nanoTime() - Proto.getLong(buf, 4)
                }
            }
        }
    }

    private fun onVideo(buf: ByteArray, len: Int, from: InetSocketAddress, fec: Boolean) {
        if (len < Proto.HEADER) return
        val now = SystemClock.elapsedRealtime()

        if (from != sender) {
            sender = from
            resetStream()
            listener.onSenderChanged(from.address.hostAddress)
        }
        lastSeenMs = now
        bytes.addAndGet(len.toLong())

        val key = (buf[3].toInt() and Proto.F_KEY) != 0
        val id = Proto.i32(buf, 4)
        val frag = Proto.u16(buf, 8) // FEC paketinde: grup no
        val count = Proto.u16(buf, 10)
        val w = Proto.u16(buf, 12)
        val h = Proto.u16(buf, 14)
        val payloadLen = len - Proto.HEADER

        if (count == 0) return
        if (fec) {
            if (frag >= (count + Proto.FEC_GROUP - 1) / Proto.FEC_GROUP) return
            if (payloadLen != 2 + Proto.PAYLOAD) return
        } else {
            if (frag >= count) return
            if (frag < count - 1 && payloadLen != Proto.PAYLOAD) return
        }
        if (haveLast && id - lastId <= 0) return // eski kare

        var slot: Slot? = null
        for (s in slots) if (s.id == id) { slot = s; break }
        if (slot == null) {
            if (slots.size >= MAX_SLOTS) slots.removeAt(0)
            slot = Slot(id, count, key, w, h, System.nanoTime())
            slots.add(slot)
        }
        if (slot.count != count) return

        val group: Int
        if (fec) {
            group = frag
            if (slot.parity[group] != null) return
            slot.parity[group] = buf.copyOfRange(Proto.HEADER + 2, Proto.HEADER + 2 + Proto.PAYLOAD)
            slot.parityLen[group] = Proto.u16(buf, Proto.HEADER)
        } else {
            if (slot.got[frag]) return
            System.arraycopy(buf, Proto.HEADER, slot.data, frag * Proto.PAYLOAD, payloadLen)
            slot.got[frag] = true
            slot.received++
            if (frag == count - 1) slot.lastLen = payloadLen
            group = frag / Proto.FEC_GROUP
            slot.groupGot[group]++
        }
        tryRecover(slot, group)

        if (slot.received == slot.count) {
            val done = slot
            slots.removeAll { it.id - done.id <= 0 }
            complete(done)
        }
    }

    /** Grupta tek paket eksikse ve eşlik paketi geldiyse eksik paketi XOR ile yeniden oluşturur. */
    private fun tryRecover(s: Slot, g: Int) {
        val first = g * Proto.FEC_GROUP
        val last = minOf(first + Proto.FEC_GROUP, s.count)
        val size = last - first
        if (size < 2 || s.groupGot[g] != size - 1) return
        val p = s.parity[g] ?: return

        var missing = -1
        for (i in first until last) if (!s.got[i]) { missing = i; break }
        if (missing < 0) return

        var lenX = s.parityLen[g]
        for (i in first until last) {
            if (i == missing) continue
            lenX = lenX xor (if (i == s.count - 1) s.lastLen else Proto.PAYLOAD)
        }
        if (lenX <= 0 || lenX > Proto.PAYLOAD) return
        if (missing != s.count - 1 && lenX != Proto.PAYLOAD) return

        val d = s.data
        val dst = missing * Proto.PAYLOAD
        System.arraycopy(p, 0, d, dst, Proto.PAYLOAD)
        for (i in first until last) {
            if (i == missing) continue
            val o = i * Proto.PAYLOAD
            for (b in 0 until Proto.PAYLOAD) d[dst + b] = (d[dst + b].toInt() xor d[o + b].toInt()).toByte()
        }
        // Son parça kısa olabilir; artan baytlar sıfır kalmalı.
        for (b in lenX until Proto.PAYLOAD) d[dst + b] = 0

        s.got[missing] = true
        s.received++
        s.groupGot[g]++
        if (missing == s.count - 1) s.lastLen = lenX
        recovered.incrementAndGet()
    }

    private fun complete(s: Slot) {
        val total = (s.count - 1) * Proto.PAYLOAD + s.lastLen
        val gap = haveLast && s.id - lastId != 1

        if (s.key) {
            waitingKey = false
        } else if (waitingKey || gap) {
            waitingKey = true
            drops.incrementAndGet()
            requestIdr()
            return
        }

        if (s.width != videoW || s.height != videoH) {
            videoW = s.width
            videoH = s.height
            listener.onVideoSize(videoW, videoH)
        }

        val ok = decoder.decode(s.data, total, s.key, s.width, s.height, s.t0)
        lastId = s.id
        haveLast = true
        if (!ok) {
            drops.incrementAndGet()
            waitingKey = true
            requestIdr()
        }
    }

    private fun resetStream() {
        slots.clear()
        haveLast = false
        waitingKey = true
        lastIdrMs = 0
        requestIdr()
    }

    private fun requestIdr() {
        val now = SystemClock.elapsedRealtime()
        if (now - lastIdrMs < 250) return
        lastIdrMs = now
        idrRequests.incrementAndGet()
        sender?.let { send(Proto.control(Proto.T_IDR_REQ), it) }
    }

    // ------------------------------------------------------------ kontrol

    private fun controlLoop() {
        val nameBytes = deviceName.toByteArray(Charsets.UTF_8).take(200).toByteArray()
        var lastStats = SystemClock.elapsedRealtime()
        while (running) {
            try {
                Thread.sleep(500)
            } catch (_: InterruptedException) {
                break
            }
            val now = SystemClock.elapsedRealtime()
            val s = sender
            if (s != null) {
                if (now - lastSeenMs > 3000) {
                    sender = null
                    listener.onSenderChanged(null)
                } else {
                    val ka = Proto.control(Proto.T_KEEPALIVE, nameBytes.size)
                    System.arraycopy(nameBytes, 0, ka, 4, nameBytes.size)
                    send(ka, s)
                    val ping = Proto.control(Proto.T_PING, 8)
                    Proto.putLong(ping, 4, System.nanoTime())
                    send(ping, s)
                }
            }
            if (now - lastStats >= 1000) {
                emitStats(now - lastStats)
                lastStats = now
            }
        }
    }

    private fun emitStats(dtMs: Long) {
        val f = frames.getAndSet(0)
        val b = bytes.getAndSet(0)
        val lc = latCount.getAndSet(0)
        val ls = latSumNs.getAndSet(0)
        val fps = f * 1000.0 / dtMs
        val mbps = b * 8.0 / (dtMs * 1000.0)
        val lat = if (lc > 0) ls / lc / 1e6 else 0.0
        val rtt = rttNs.let { if (it >= 0) String.format(Locale.US, "%.1f ms", it / 1e6) else "-" }
        listener.onStats(
            String.format(
                Locale.US,
                "%dx%d  %.0f fps  %.1f Mbps\nalım→ekran %.1f ms  RTT %s\natılan kare %d  IDR isteği %d  FEC kurtarma %d",
                videoW, videoH, fps, mbps, lat, rtt, drops.get(), idrRequests.get(), recovered.get(),
            )
        )
    }

    private fun send(data: ByteArray, to: InetSocketAddress) {
        try {
            socket?.send(DatagramPacket(data, data.size, to))
        } catch (_: Exception) {
        }
    }

    // ---------------------------------------------------------------- keşif

    private fun beaconLoop() {
        val sock = try {
            DatagramSocket().apply { broadcast = true }
        } catch (_: Exception) {
            return
        }
        val msg = "YANSI1|$deviceName|${Proto.VIDEO_PORT}".toByteArray(Charsets.UTF_8)
        while (running) {
            for (addr in broadcastAddresses()) {
                try {
                    sock.send(DatagramPacket(msg, msg.size, addr, Proto.DISCOVERY_PORT))
                } catch (_: Exception) {
                }
            }
            try {
                Thread.sleep(1000)
            } catch (_: InterruptedException) {
                break
            }
        }
        sock.close()
    }

    private fun broadcastAddresses(): Set<InetAddress> {
        val out = LinkedHashSet<InetAddress>()
        try {
            for (ni in Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!ni.isUp || ni.isLoopback) continue
                for (ia in ni.interfaceAddresses) ia.broadcast?.let { out.add(it) }
            }
        } catch (_: Exception) {
        }
        try {
            out.add(InetAddress.getByName("255.255.255.255"))
        } catch (_: Exception) {
        }
        return out
    }

    companion object {
        private const val MAX_SLOTS = 4
    }
}
