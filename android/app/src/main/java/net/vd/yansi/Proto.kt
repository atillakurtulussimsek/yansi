package net.vd.yansi

/** Protokol sabitleri — ayrıntı için PROTOCOL.md. */
object Proto {
    const val VIDEO_PORT = 47800
    const val DISCOVERY_PORT = 47801

    const val MAGIC0: Byte = 0x59 // 'Y'
    const val MAGIC1: Byte = 0x53 // 'S'

    const val T_VIDEO = 0x01
    const val T_IDR_REQ = 0x10
    const val T_KEEPALIVE = 0x11
    const val T_PING = 0x12
    const val T_PONG = 0x13

    const val F_KEY = 0x01

    const val HEADER = 16
    const val PAYLOAD = 1200

    fun u16(b: ByteArray, o: Int): Int =
        ((b[o].toInt() and 0xFF) shl 8) or (b[o + 1].toInt() and 0xFF)

    fun i32(b: ByteArray, o: Int): Int = (u16(b, o) shl 16) or u16(b, o + 2)

    fun putLong(b: ByteArray, o: Int, v: Long) {
        for (i in 0 until 8) b[o + i] = (v ushr (56 - 8 * i)).toByte()
    }

    fun getLong(b: ByteArray, o: Int): Long {
        var v = 0L
        for (i in 0 until 8) v = (v shl 8) or (b[o + i].toLong() and 0xFF)
        return v
    }

    fun control(type: Int, payloadSize: Int = 0): ByteArray =
        ByteArray(4 + payloadSize).also {
            it[0] = MAGIC0
            it[1] = MAGIC1
            it[2] = type.toByte()
            it[3] = 0
        }
}
