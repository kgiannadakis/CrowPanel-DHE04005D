#if HAS_TFT && USE_MCUI

#include "McSender.h"
#include "configuration.h"

#include "concurrency/OSThread.h"
#include "mesh/MeshService.h"
#include "mesh/MeshTypes.h"
#include "mesh/NextHopRouter.h"
#include "mesh/NodeDB.h"
#include "mesh/ReliableRouter.h"
#include "mesh/Router.h"
#include "mesh/generated/meshtastic/portnums.pb.h"
#include "modules/NodeInfoModule.h"

#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <time.h>

namespace mcui {

struct PendingSend {
    McConvId id;
    size_t len;
    char text[228];
};

static constexpr int SEND_QUEUE_DEPTH = 4;
static PendingSend s_queue[SEND_QUEUE_DEPTH];
static int s_q_head = 0;
static int s_q_count = 0;
static SemaphoreHandle_t s_q_lock = nullptr;

struct PendingAck {
    uint32_t packet_id;
    uint32_t from_node;
    McConvId conv;
    uint32_t first_seen_ms;
    bool observed_pending;
    bool finalized;
};
static constexpr int PENDING_ACK_MAX = 8;
static constexpr uint32_t PENDING_TIMEOUT_MS = 120000;
static PendingAck s_pending[PENDING_ACK_MAX];
static uint32_t s_last_key_request_ms = 0;

static bool direct_key_available(NodeNum node)
{
#if !(MESHTASTIC_EXCLUDE_PKI)
    if (!nodeDB || node == 0 || node == nodeDB->getNodeNum()) {
        return true;
    }
    meshtastic_NodeInfoLite *n = nodeDB->getMeshNode(node);
    return n && n->has_user && n->user.public_key.size == 32;
#else
    (void)node;
    return true;
#endif
}

static void request_key_exchange()
{
#if !(MESHTASTIC_EXCLUDE_PKI)
    uint32_t now = millis();
    if (nodeInfoModule && (s_last_key_request_ms == 0 || (now - s_last_key_request_ms) > 60000)) {
        LOG_INFO("mcui: requesting NodeInfo/public keys for direct messages");
        nodeInfoModule->sendOurNodeInfo(NODENUM_BROADCAST, true, 0, true);
        s_last_key_request_ms = now;
    }
#endif
}

static bool enqueue(const PendingSend &ps)
{
    if (!s_q_lock) return false;
    bool ok = false;
    xSemaphoreTake(s_q_lock, portMAX_DELAY);
    if (s_q_count < SEND_QUEUE_DEPTH) {
        int idx = (s_q_head + s_q_count) % SEND_QUEUE_DEPTH;
        s_queue[idx] = ps;
        s_q_count++;
        ok = true;
    }
    xSemaphoreGive(s_q_lock);
    return ok;
}

static bool dequeue(PendingSend &out)
{
    if (!s_q_lock) return false;
    bool ok = false;
    xSemaphoreTake(s_q_lock, portMAX_DELAY);
    if (s_q_count > 0) {
        out = s_queue[s_q_head];
        s_q_head = (s_q_head + 1) % SEND_QUEUE_DEPTH;
        s_q_count--;
        ok = true;
    }
    xSemaphoreGive(s_q_lock);
    return ok;
}

static void pending_add(uint32_t packet_id, uint32_t from_node, const McConvId &conv)
{

    for (int i = 0; i < PENDING_ACK_MAX; i++) {
        if (s_pending[i].packet_id == 0) {
            s_pending[i].packet_id = packet_id;
            s_pending[i].from_node = from_node;
            s_pending[i].conv = conv;
            s_pending[i].first_seen_ms = millis();
            s_pending[i].observed_pending = false;
            s_pending[i].finalized = false;
            return;
        }
    }

    int oldest = 0;
    for (int i = 1; i < PENDING_ACK_MAX; i++) {
        if (s_pending[i].first_seen_ms < s_pending[oldest].first_seen_ms) {
            oldest = i;
        }
    }
    LOG_WARN("mcui: pending-ack table full; evicting oldest id=%u to track id=%u",
             (unsigned)s_pending[oldest].packet_id, (unsigned)packet_id);
    messages_mark_ack_by_packet_id(s_pending[oldest].packet_id, false);

    s_pending[oldest].packet_id = packet_id;
    s_pending[oldest].from_node = from_node;
    s_pending[oldest].conv = conv;
    s_pending[oldest].first_seen_ms = millis();
    s_pending[oldest].observed_pending = false;
    s_pending[oldest].finalized = false;
}

static void on_ack_nak_received(NodeNum from, PacketId id, bool isAck)
{

    (void)from;
    for (int i = 0; i < PENDING_ACK_MAX; i++) {
        PendingAck &e = s_pending[i];
        if (e.packet_id != id) continue;
        LOG_INFO("mcui: %s observed for id=%u", isAck ? "ACK" : "NAK", (unsigned)id);
        messages_mark_ack_by_packet_id(e.packet_id, isAck);
        e.finalized = true;

        return;
    }

}

static void do_send(const PendingSend &ps)
{
    if (!router || !service) {
        LOG_WARN("mcui: send dropped — router/service null");

        if (ps.id.kind == McConvId::DIRECT) {
            messages_mark_last_unsent_as_failed(ps.id);
        }
        return;
    }

    if (ps.id.kind == McConvId::DIRECT && !direct_key_available((NodeNum)ps.id.value)) {
        LOG_WARN("mcui: direct send blocked; missing public key for 0x%08x", (unsigned)ps.id.value);
        request_key_exchange();
        messages_mark_last_unsent_as_failed(ps.id);
        return;
    }

    meshtastic_MeshPacket *p = router->allocForSending();
    if (!p) {
        LOG_WARN("mcui: send dropped — allocForSending returned null");
        if (ps.id.kind == McConvId::DIRECT) {
            messages_mark_last_unsent_as_failed(ps.id);
        }
        return;
    }

    if (ps.id.kind == McConvId::DIRECT) {
        p->to = ps.id.value;
        p->want_ack = true;
        p->channel = 0;
    } else {
        p->to = NODENUM_BROADCAST;
        p->want_ack = false;
        p->channel = (uint8_t)ps.id.value;
    }
    p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    p->decoded.payload.size = ps.len;
    memcpy(p->decoded.payload.bytes, ps.text, ps.len);

    const uint32_t sent_id = p->id;
    const uint32_t sent_from = p->from ? p->from : (nodeDB ? nodeDB->getNodeNum() : 0);

    service->sendToMesh(p, RX_SRC_LOCAL, true);

    messages_attach_packet_id(ps.id, sent_id);

    if (ps.id.kind == McConvId::DIRECT) {
        pending_add(sent_id, sent_from, ps.id);
    }

    LOG_INFO("mcui: sent %u bytes to %s %u (id=%u)",
             (unsigned)ps.len,
             ps.id.kind == McConvId::DIRECT ? "node" : "ch",
             (unsigned)ps.id.value,
             (unsigned)sent_id);
}

static void poll_pending_acks()
{
    if (!router) return;

    NextHopRouter *nhr = static_cast<NextHopRouter *>(router);

    uint32_t now = millis();
    for (int i = 0; i < PENDING_ACK_MAX; i++) {
        PendingAck &e = s_pending[i];
        if (e.packet_id == 0) continue;

        if (e.finalized) {

            e.packet_id = 0;
            continue;
        }

        bool pending = nhr->isAwaitingAck(e.from_node, e.packet_id);
        if (pending) {
            e.observed_pending = true;

        } else if (e.observed_pending) {

            LOG_INFO("mcui: retries exhausted for id=%u -> failed",
                     (unsigned)e.packet_id);
            messages_mark_ack_by_packet_id(e.packet_id, false);
            e.packet_id = 0;
            continue;
        }

        if ((now - e.first_seen_ms) >= PENDING_TIMEOUT_MS) {
            LOG_WARN("mcui: ack timeout id=%u -> failed", (unsigned)e.packet_id);
            messages_mark_ack_by_packet_id(e.packet_id, false);
            e.packet_id = 0;
        }
    }
}

class McSendThread : public concurrency::OSThread
{
  public:
    McSendThread() : concurrency::OSThread("McSender") {}

  protected:
    int32_t runOnce() override
    {
        PendingSend ps;
        while (dequeue(ps)) {
            do_send(ps);
        }

        poll_pending_acks();
        return 200;
    }
};

static McSendThread *s_thread = nullptr;

void sender_init()
{
    if (!s_q_lock) s_q_lock = xSemaphoreCreateMutex();
    if (!s_thread) s_thread = new McSendThread();

    g_ackNakObserver = &on_ack_nak_received;
}

bool sender_send_text(const McConvId &id, const char *text)
{
    if (!text || !text[0]) return false;

    sender_init();
    if (!s_q_lock) return false;

    size_t len = strlen(text);
    if (len > sizeof(((PendingSend *)nullptr)->text)) {
        len = sizeof(((PendingSend *)nullptr)->text);
    }

    PendingSend ps = {};
    ps.id = id;
    ps.len = len;
    memcpy(ps.text, text, len);
    if (!enqueue(ps)) {
        LOG_WARN("mcui: send queue full, dropping message");
        return false;
    }
    concurrency::mainDelay.interrupt();

    McMessage m = {};
    m.from_node = 0;
    m.timestamp = (uint32_t)time(nullptr);
    m.outgoing = true;
    m.delivered = (id.kind != McConvId::DIRECT);
    m.packet_id = 0;
    strncpy(m.text, text, sizeof(m.text) - 1);
    messages_append(id, m);

    return true;
}

}

#endif
