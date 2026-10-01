package com.thorkracing.dmdLora.radio

/**
 * A bridge that drops within a minute of connecting and stays away. So far that has always been a
 * hung board, never a parked bike: twice on 2026-10-01 the E220 bridge went silent 22-26 s after
 * power-up - the engine start, by the timing - and came back only when re-plugged, after 42 and
 * 25 minutes of riding without it. A bike switched off at a stop drops a bridge that has been up
 * for longer than that.
 */
class EarlyDrop(private val earlyMs: Long = 60_000L, private val waitMs: Long = 30_000L) {
    private var upSinceMs = 0L
    private var droppedAtMs = 0L
    private var told = false

    /** Feed the link state every tick. True once per such drop, when it is time to tell the rider. */
    fun update(connected: Boolean, nowMs: Long): Boolean {
        if (connected) {
            if (upSinceMs == 0L) upSinceMs = nowMs
            droppedAtMs = 0L
            told = false
            return false
        }
        if (upSinceMs != 0L) {
            droppedAtMs = if (nowMs - upSinceMs < earlyMs) nowMs else 0L
            upSinceMs = 0L
        }
        if (droppedAtMs == 0L || told || nowMs - droppedAtMs < waitMs) return false
        told = true
        return true
    }
}
