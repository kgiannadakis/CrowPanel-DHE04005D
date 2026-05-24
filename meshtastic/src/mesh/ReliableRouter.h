#pragma once

#include "NextHopRouter.h"

/**
 * Optional observer hook for ACK/NAK arrivals.
 *
 * When a Routing packet addressed to us lands that references a pending
 * outgoing packet id (via request_id), ReliableRouter::sniffReceived will
 * invoke this callback BEFORE the pending retransmission entry is removed.
 *
 * - `from` is the node the ACK/NAK was destined to (i.e. us, the original
 *    sender of the packet being acked)
 * - `id` is the original outgoing packet id
 * - `isAck` is true when error_reason == Routing_Error_NONE, false for any
 *    NAK error code
 *
 * Used by the mcui layer to distinguish real ACKs from retry exhaustion so
 * the chat bubble can show "acknowledged" vs "failed". Only one observer is
 * supported; mcui registers it at startup. Left null when mcui is disabled.
 */
typedef void (*AckNakObserverFn)(NodeNum from, PacketId id, bool isAck);
extern AckNakObserverFn g_ackNakObserver;

/**
 * This is a mixin that extends Router with the ability to do (one hop only) reliable message sends.
 */
class ReliableRouter : public NextHopRouter
{
  public:
    /**
     * Constructor
     *
     */
    // ReliableRouter();

    /**
     * Send a packet on a suitable interface.  This routine will
     * later free() the packet to pool.  This routine is not allowed to stall.
     * If the txmit queue is full it might return an error
     */
    virtual ErrorCode send(meshtastic_MeshPacket *p) override;

  protected:
    /**
     * Look for acks/naks or someone retransmitting us
     */
    virtual void sniffReceived(const meshtastic_MeshPacket *p, const meshtastic_Routing *c) override;

    /**
     * We hook this method so we can see packets before FloodingRouter says they should be discarded
     */
    virtual bool shouldFilterReceived(const meshtastic_MeshPacket *p) override;

  private:
    /**
     * Should this packet be ACKed with a want_ack for reliable delivery?
     */
    bool shouldSuccessAckWithWantAck(const meshtastic_MeshPacket *p);
};