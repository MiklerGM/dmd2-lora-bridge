package com.thorkracing.dmdLora.radio

/**
 * Whether the pairing with the bridge is still any good.
 *
 * A stale key does not announce itself. The GATT handshake completes normally and only the
 * encryption fails, several seconds later, when the first command is written — so a session
 * that reached "ready" and then carried nothing is a failure just as much as one that never
 * connected. Android will not drop a key of its own accord, so something has to decide when to,
 * and both mistakes are expensive: too eager puts a pairing dialog in front of a rider mid-ride,
 * too slow leaves a pair that cannot recover without a human and a settings screen.
 *
 * One dead session is not enough to judge on — connections drop for dull reasons — so it takes
 * a run of them.
 */
class BondHealth(private val attempts: Int = 2) {

    private var deadSessions = 0

    /**
     * A link ended, and [carriedData] says whether anything at all arrived over it. That is
     * the only proof the key we hold is the one the bridge holds, and it is enough on its own:
     * a mid-session reset would be undone by nothing, since the session it belongs to ends by
     * reporting the same fact.
     *
     * True when the pairing should be cleared and adopted afresh.
     */
    fun sessionEnded(carriedData: Boolean): Boolean {
        if (carriedData) {
            deadSessions = 0
            return false
        }
        if (++deadSessions < attempts) return false
        deadSessions = 0
        return true
    }
}
