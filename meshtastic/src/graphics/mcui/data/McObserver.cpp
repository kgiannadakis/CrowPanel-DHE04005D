#if HAS_TFT && USE_MCUI

#include "McObserver.h"
#include "McMessages.h"
#include "configuration.h"
#include "crowpanel_backlight.h"

#include "Observer.h"
#include "mesh/MeshTypes.h"
#include "mesh/NodeDB.h"
#include "modules/NodeInfoModule.h"
#include "modules/TextMessageModule.h"

#include <Throttle.h>
#include <cstring>

namespace mcui {

class TextObserver : public Observer<const meshtastic_MeshPacket *>
{
  public:
    int onNotify(const meshtastic_MeshPacket *mp) override
    {
        LOG_DEBUG("mcui: observer onNotify from=0x%x to=0x%x port=%d",
                  mp ? (unsigned)mp->from : 0,
                  mp ? (unsigned)mp->to : 0,
                  mp ? (int)mp->decoded.portnum : -1);
        if (!mp || !nodeDB) return 0;

        if (mp->rx_rssi != 0) node_rssi_set(mp->from, (int16_t)mp->rx_rssi);

        if (mp->decoded.portnum != meshtastic_PortNum_TEXT_MESSAGE_APP) return 0;

#if !(MESHTASTIC_EXCLUDE_PKI)
        static uint32_t last_key_request_ms = 0;
        meshtastic_NodeInfoLite *sender = nodeDB->getMeshNode(mp->from);
        bool missing_key = !sender || sender->user.public_key.size != 32;
        if (mp->from != nodeDB->getNodeNum() && missing_key && nodeInfoModule &&
            !Throttle::isWithinTimespanMs(last_key_request_ms, 60000)) {
            LOG_INFO("mcui: heard node 0x%08x without public key; requesting NodeInfo", (unsigned)mp->from);
            nodeInfoModule->sendOurNodeInfo(NODENUM_BROADCAST, true, 0, true);
            last_key_request_ms = millis();
        }
#endif

        McMessage m = {};
        m.from_node = mp->from;
        m.timestamp = mp->rx_time ? mp->rx_time : (uint32_t)time(nullptr);
        m.snr = mp->rx_snr;
        m.rssi = mp->rx_rssi;
        m.outgoing = false;
        m.delivered = false;
        size_t n = mp->decoded.payload.size;
        if (n >= sizeof(m.text)) n = sizeof(m.text) - 1;
        memcpy(m.text, mp->decoded.payload.bytes, n);
        m.text[n] = '\0';

        NodeNum ours = nodeDB->getNodeNum();
        McConvId id;
        if (mp->to == NODENUM_BROADCAST) {
            id = McConvId::channel(mp->channel);
        } else if (mp->to == ours) {

            id = McConvId::direct(mp->from);
        } else {

            return 0;
        }
        messages_append(id, m);
        LOG_INFO("mcui: rx text %u bytes from 0x%x on %s %u",
                 (unsigned)n, (unsigned)mp->from,
                 id.kind == McConvId::DIRECT ? "node" : "ch",
                 (unsigned)id.value);

        backlight_notify_activity();
        return 0;
    }
};

static TextObserver *s_observer = nullptr;
static bool s_attached = false;

void observer_init()
{
    if (s_attached) return;
    if (!textMessageModule) {

        return;
    }
    if (!s_observer) s_observer = new TextObserver();
    s_observer->observe(textMessageModule);
    s_attached = true;
    LOG_INFO("mcui: text message observer attached");
}

bool observer_attached()
{
    return s_attached;
}

}

#endif
