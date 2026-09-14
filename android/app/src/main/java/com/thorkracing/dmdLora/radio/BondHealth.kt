package com.thorkracing.dmdLora.radio

/**
 * Whether the pairing with the bridge is still any good.
 *
 * A stale key does not announce itself. The GATT handshake completes normally and only the
 * encryption fails, several seconds later, when the first command is written. Notifications
 * from the bridge are deliberately open, so receiving one proves nothing about the key; only
 * a successful write to its encrypted characteristic does. Android will not drop a stale key
 * of its own accord, so something has to decide when to, and both mistakes are expensive: too
 * eager puts a pairing dialog in front of a rider mid-ride, too slow leaves a pair that cannot
 * recover without a human and a settings screen.
 *
 * One dead session is not enough to judge on — connections drop for dull reasons — so it takes
 * a run of them.
 */
class BondHealth(private val attempts: Int = 2) {

    private var deadSessions = 0

    /** A successful protected write proves the stored keys still agree. */
    fun authenticatedWriteSucceeded() {
        deadSessions = 0
    }

    /** Suspicion belongs to one peer; carrying it to another board is a false accusation. */
    fun peerChanged() {
        deadSessions = 0
    }

    /**
     * True after a run of sessions where a bonded peer rejected a protected write. A generic
     * connect, discovery or timeout failure is not evidence against the key and does not count.
     */
    fun sessionEnded(authenticationRejected: Boolean): Boolean {
        if (!authenticationRejected) return false
        if (++deadSessions < attempts) return false
        deadSessions = 0
        return true
    }
}
