package com.thorkracing.dmdLora.protocol

import java.security.MessageDigest
import java.util.Base64
import java.util.zip.CRC32

/**
 * DMD2 LoRa group-ride packet codec. See docs/PROTOCOL.md.
 *
 * Pure JVM on purpose: no Android APIs, so it is unit-testable on the desktop.
 */
object Codec {
    const val MAGIC: Int = 0xD2
    const val VERSION: Int = 3
    const val DEFAULT_TTL: Int = 2

    /**
     * What we send. The stock app sends 2 and relays anything above 1 when it hears two or more
     * other senders - with the TTL decremented and the CRC left stale, so no receiver that missed
     * the original can read the relay. Each of those relays also pushes its own position back
     * by a whole interval, and a tablet in a busy group can end up sending little but relays.
     * TTL 1 keeps our packets out of that.
     */
    const val TX_TTL: Int = 1
    const val TYPE_GROUP: Int = 0
    const val TYPE_CHAT: Int = 1
    const val MAX_RIDERS: Int = 9
    const val HEADER_LEN: Int = 12
    const val RIDER_LEN: Int = 14

    // status bits
    const val ST_SHARING = 1
    const val ST_NEEDS_HELP = 2
    const val ST_TIMED_OUT = 4
    const val ST_HAS_INTERNET = 8
    const val ST_HAS_LORA = 16
    const val ST_SERVER_RELAY = 32

    private const val LAT_SCALE = 46603.375
    private const val LON_SCALE = 23301.6875

    /** CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflect, no xorout. */
    fun crc16(data: ByteArray, len: Int = data.size): Int {
        var crc = 0xFFFF
        for (i in 0 until len) {
            crc = crc xor ((data[i].toInt() and 0xFF) shl 8)
            repeat(8) {
                crc = if (crc and 0x8000 != 0) ((crc shl 1) xor 0x1021) else (crc shl 1)
                crc = crc and 0xFFFF
            }
        }
        return crc
    }

    /** 16-bit rider identity on the wire. */
    fun uidHash(userId: String): Int = crc16(userId.toByteArray(Charsets.UTF_8))

    /** 32-bit group selector. */
    fun groupHash(groupId: String): Long {
        val c = CRC32()
        c.update(groupId.toByteArray(Charsets.UTF_8))
        return c.value
    }

    /** 16-byte XOR obfuscation pad. Not encryption — see docs/PROTOCOL.md. */
    fun groupKey(groupId: String): ByteArray =
        MessageDigest.getInstance("SHA-256")
            .digest(groupId.toByteArray(Charsets.UTF_8))
            .copyOf(16)

    fun toWire(packet: ByteArray): String =
        Base64.getEncoder().withoutPadding().encodeToString(packet)

    fun fromWire(wire: String): ByteArray {
        val pad = (4 - wire.length % 4) % 4
        return Base64.getDecoder().decode(wire + "=".repeat(pad))
    }

    fun encodeGroup(
        groupId: String,
        senderUserId: String,
        riders: List<TxRider>,
        seq: Int,
        ttl: Int = DEFAULT_TTL,
    ): ByteArray {
        val take = riders.take(MAX_RIDERS)
        val buf = Buf(HEADER_LEN + take.size * RIDER_LEN + 2)
        header(buf, groupId, senderUserId, seq, TYPE_GROUP, ttl)
        for (r in take) {
            buf.u16(uidHash(r.userId))
            // Out-of-range or non-finite input must not alias into a plausible position:
            // the 24-bit fields wrap, so an unclamped value lands somewhere real-looking.
            buf.u24(encodeLat(r.lat))
            buf.u24(encodeLon(r.lon))
            buf.u16(((r.speedKmh * 10.0).toIntClamped()).coerceIn(0, 0xFFFF))
            buf.u8(r.status)
            buf.u24(r.positionAgeS.coerceIn(0, 0xFFFFFF))
        }
        return finish(buf, groupId)
    }

    fun encodeChat(
        groupId: String,
        senderUserId: String,
        senderName: String,
        text: String,
        seq: Int,
        ttl: Int = DEFAULT_TTL,
    ): ByteArray {
        val name = truncateUtf8(senderName, 31)
        val msg = truncateUtf8(text, 135 - name.size)
        val buf = Buf(HEADER_LEN + 1 + name.size + msg.size + 2)
        header(buf, groupId, senderUserId, seq, TYPE_CHAT, ttl)
        buf.u8(name.size)
        buf.bytes(name)
        buf.bytes(msg)
        return finish(buf, groupId)
    }

    fun decode(packet: ByteArray, groupId: String, ownUidHash: Int? = null): Decoded {
        if (packet.size < HEADER_LEN + 2) return Decoded.Error("TOO_SHORT")
        if (packet[0].toInt() and 0xFF != MAGIC) return Decoded.Error("WRONG_MAGIC")
        val version = packet[1].toInt() and 0xFF
        if (version != 2 && version != 3) return Decoded.Error("WRONG_VERSION($version)")
        val ttl = packet[2].toInt() and 0xFF
        val type = packet[3].toInt() and 0xFF
        val senderHash = be16(packet, 4)
        val seq = be16(packet, 6)
        val gHash = be32(packet, 8)

        if (ownUidHash != null && senderHash == ownUidHash) return Decoded.Error("SELF")
        if (gHash != groupHash(groupId)) return Decoded.Error("WRONG_GROUP")

        val end = packet.size - 2
        val want = be16(packet, end)
        val buf = packet.copyOf()
        xorBody(buf, end, groupKey(groupId))
        // A stock relay still carries the CRC of the TTL it was first sent with, one higher.
        // Checking against that too turns a relay nobody could read into one we can.
        val relayed = crc16(buf, end) != want
        if (relayed) {
            buf[2] = (ttl + 1).toByte()
            if (crc16(buf, end) != want) return Decoded.Error("CRC_MISMATCH")
        }

        if (type and 1 != 0) {
            val nameLen = buf[HEADER_LEN].toInt() and 0xFF
            if (nameLen > buf.size - (HEADER_LEN + 3)) return Decoded.Error("CHAT_NAME_LEN_INVALID")
            val nameStart = HEADER_LEN + 1
            return Decoded.Chat(
                version, ttl, senderHash, seq, gHash,
                senderName = String(buf, nameStart, nameLen, Charsets.UTF_8),
                text = String(buf, nameStart + nameLen, end - nameStart - nameLen, Charsets.UTF_8),
                relayed = relayed,
            )
        }

        val body = packet.size - (HEADER_LEN + 2)
        if (body < 0 || body % RIDER_LEN != 0) return Decoded.Error("BAD_RIDER_LEN($body)")
        val riders = ArrayList<RxRider>(body / RIDER_LEN)
        for (i in 0 until body / RIDER_LEN) {
            val o = HEADER_LEN + i * RIDER_LEN
            riders += RxRider(
                userIdHash = be16(buf, o),
                lat = round6(be24(buf, o + 2) / LAT_SCALE - 90.0),
                lon = round6(be24(buf, o + 5) / LON_SCALE - 180.0),
                speedKmh = be16(buf, o + 8) / 10.0,
                status = buf[o + 10].toInt() and 0xFF,
                positionAgeS = be24(buf, o + 11),
            )
        }
        return Decoded.Group(version, ttl, senderHash, seq, gHash, riders, relayed)
    }

    /**
     * The same packet under another sequence number. A position is encoded when DMD2 hands it
     * over but may be held or superseded by pacing, so it takes its number only when it is sent:
     * then consecutive numbers are consecutive transmissions, and a receiver can count the gaps.
     */
    fun withSeq(packet: ByteArray, groupId: String, seq: Int): ByteArray {
        val end = packet.size - 2
        val plain = packet.copyOf()
        xorBody(plain, end, groupKey(groupId))
        plain[6] = (seq shr 8).toByte()
        plain[7] = seq.toByte()
        val crc = crc16(plain, end)
        return packet.copyOf().also {
            it[6] = plain[6]
            it[7] = plain[7]
            it[end] = (crc shr 8).toByte()
            it[end + 1] = crc.toByte()
        }
    }

    // ---- internals ----

    private fun header(buf: Buf, groupId: String, sender: String, seq: Int, type: Int, ttl: Int) {
        buf.u8(MAGIC); buf.u8(VERSION); buf.u8(ttl); buf.u8(type)
        buf.u16(if (sender.isEmpty()) 0 else uidHash(sender))
        buf.u16(seq and 0xFFFF)
        buf.u32(groupHash(groupId))
    }

    /** CRC covers the plaintext; only the body is then obfuscated. Order matters. */
    private fun finish(buf: Buf, groupId: String): ByteArray {
        val end = buf.pos
        buf.u16(crc16(buf.array, end))
        val out = buf.array
        xorBody(out, end, groupKey(groupId))
        return out
    }

    private fun xorBody(buf: ByteArray, end: Int, key: ByteArray) {
        for (i in HEADER_LEN until end) {
            buf[i] = (buf[i].toInt() xor key[(i - HEADER_LEN) % key.size].toInt()).toByte()
        }
    }

    private fun truncateUtf8(s: String, max: Int): ByteArray {
        if (max <= 0) return ByteArray(0)
        val b = s.toByteArray(Charsets.UTF_8)
        if (b.size <= max) return b
        var n = max
        while (n > 0 && (b[n].toInt() and 0xC0) == 0x80) n--
        return b.copyOf(n)
    }

    /** NaN would silently become 0, which decodes as a rider sitting at -90/-180. */
    private fun Double.toIntClamped(): Int = when {
        isNaN() -> 0
        this <= Int.MIN_VALUE.toDouble() -> Int.MIN_VALUE
        this >= Int.MAX_VALUE.toDouble() -> Int.MAX_VALUE
        else -> toInt()
    }

    // Truncated, as the stock app does, so our packets match its reference vectors byte for byte.
    // A decode/encode round trip therefore moves about half of all points by one LSB (2-3 m);
    // RetoldAge matches points with that tolerance.
    internal fun encodeLat(lat: Double): Int =
        (((if (lat.isNaN()) 0.0 else lat.coerceIn(-90.0, 90.0)) + 90.0) * LAT_SCALE)
            .toIntClamped().coerceIn(0, 0xFFFFFF)

    internal fun encodeLon(lon: Double): Int =
        (((if (lon.isNaN()) 0.0 else lon.coerceIn(-180.0, 180.0)) + 180.0) * LON_SCALE)
            .toIntClamped().coerceIn(0, 0xFFFFFF)

    private fun round6(v: Double): Double = Math.round(v * 1_000_000.0) / 1_000_000.0
    private fun be16(b: ByteArray, o: Int) = ((b[o].toInt() and 0xFF) shl 8) or (b[o + 1].toInt() and 0xFF)
    private fun be24(b: ByteArray, o: Int) =
        ((b[o].toInt() and 0xFF) shl 16) or ((b[o + 1].toInt() and 0xFF) shl 8) or (b[o + 2].toInt() and 0xFF)
    private fun be32(b: ByteArray, o: Int): Long =
        ((be16(b, o).toLong() shl 16) or be16(b, o + 2).toLong()) and 0xFFFFFFFFL

    private class Buf(size: Int) {
        val array = ByteArray(size)
        var pos = 0
        fun u8(v: Int) { array[pos++] = (v and 0xFF).toByte() }
        fun u16(v: Int) { u8(v shr 8); u8(v) }
        fun u24(v: Int) { u8(v shr 16); u8(v shr 8); u8(v) }
        fun u32(v: Long) { u8((v shr 24).toInt()); u8((v shr 16).toInt()); u8((v shr 8).toInt()); u8(v.toInt()) }
        fun bytes(b: ByteArray) { b.copyInto(array, pos); pos += b.size }
    }
}
