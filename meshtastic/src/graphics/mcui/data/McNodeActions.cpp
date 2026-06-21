#if HAS_TFT && USE_MCUI

#include "McNodeActions.h"

#include "configuration.h"
#include "concurrency/OSThread.h"
#include "mesh/NodeDB.h"

#if !MESHTASTIC_EXCLUDE_TRACEROUTE
#include "modules/TraceRouteModule.h"
#endif
#include "modules/NodeInfoModule.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace mcui {

enum class NodeActionType : uint8_t {
    None,
    ClearAll,
    Delete,
    SetFavorite,
    TraceRoute,
    RequestUserInfo,
};

struct PendingNodeAction {
    NodeActionType type = NodeActionType::None;
    NodeNum node = 0;
    bool favorite = false;
};

static constexpr int ACTION_QUEUE_MAX = 8;
static PendingNodeAction s_queue[ACTION_QUEUE_MAX];
static uint8_t s_head = 0;
static uint8_t s_tail = 0;
static SemaphoreHandle_t s_lock = nullptr;
static volatile uint32_t s_change_tick = 1;

static void lock_init()
{
    if (!s_lock)
        s_lock = xSemaphoreCreateMutex();
}

static bool queue_push(PendingNodeAction action)
{
    lock_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint8_t next = (uint8_t)((s_tail + 1) % ACTION_QUEUE_MAX);
    if (next == s_head) {
        xSemaphoreGive(s_lock);
        LOG_WARN("mcui: node action queue full");
        return false;
    }
    s_queue[s_tail] = action;
    s_tail = next;
    xSemaphoreGive(s_lock);
    return true;
}

static bool queue_pop(PendingNodeAction &action)
{
    lock_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_head == s_tail) {
        xSemaphoreGive(s_lock);
        return false;
    }
    action = s_queue[s_head];
    s_queue[s_head] = {};
    s_head = (uint8_t)((s_head + 1) % ACTION_QUEUE_MAX);
    xSemaphoreGive(s_lock);
    return true;
}

class McNodeActionThread : public concurrency::OSThread
{
  public:
    McNodeActionThread() : concurrency::OSThread("McNodeAct") {}

  protected:
    int32_t runOnce() override
    {
        PendingNodeAction action;
        if (!queue_pop(action))
            return 250;

        bool changed = false;
        switch (action.type) {
        case NodeActionType::ClearAll:
            if (nodeDB) {
                LOG_INFO("mcui: clearing node database");
                nodeDB->resetNodes(false);
                changed = true;
            }
            break;
        case NodeActionType::Delete:
            if (nodeDB && action.node != 0 && action.node != nodeDB->getNodeNum()) {
                LOG_INFO("mcui: deleting node 0x%08x", action.node);
                nodeDB->removeNodeByNum(action.node);
                changed = true;
            }
            break;
        case NodeActionType::SetFavorite:
            if (nodeDB && action.node != 0 && action.node != nodeDB->getNodeNum()) {
                LOG_INFO("mcui: setting node 0x%08x favorite=%d", action.node, action.favorite);
                nodeDB->set_favorite(action.favorite, action.node);
                changed = true;
            }
            break;
        case NodeActionType::TraceRoute:
#if !MESHTASTIC_EXCLUDE_TRACEROUTE
            if (traceRouteModule && action.node != 0 && (!nodeDB || action.node != nodeDB->getNodeNum())) {
                LOG_INFO("mcui: starting traceroute to 0x%08x", action.node);
                traceRouteModule->startTraceRoute(action.node);
            } else {
                LOG_WARN("mcui: traceroute unavailable for 0x%08x", action.node);
            }
#else
            LOG_WARN("mcui: traceroute module excluded");
#endif
            break;
        case NodeActionType::RequestUserInfo:
            if (nodeInfoModule && action.node != 0 && (!nodeDB || action.node != nodeDB->getNodeNum())) {
                // Send our NodeInfo to the target with want_response set so it replies with its
                // own User record (long/short name). shorterTimeout marks this as an interactive,
                // user-initiated request (60s throttle instead of the ~10 min broadcast cadence).
                const meshtastic_NodeInfoLite *info = nodeDB ? nodeDB->getMeshNode(action.node) : nullptr;
                uint8_t channel = info ? info->channel : 0;
                LOG_INFO("mcui: requesting user info from 0x%08x on ch %u", action.node, (unsigned)channel);
                nodeInfoModule->sendOurNodeInfo(action.node, true /* wantReplies */, channel, true /* shorterTimeout */);
            } else {
                LOG_WARN("mcui: nodeinfo request unavailable for 0x%08x", action.node);
            }
            break;
        case NodeActionType::None:
            break;
        }

        if (changed)
            s_change_tick++;
        return 10;
    }
};

static McNodeActionThread *s_thread = nullptr;

void node_actions_init()
{
    lock_init();
    if (!s_thread)
        s_thread = new McNodeActionThread();
}

void node_actions_clear_all()
{
    node_actions_init();
    queue_push({NodeActionType::ClearAll, 0, false});
}

void node_actions_delete(NodeNum node)
{
    node_actions_init();
    queue_push({NodeActionType::Delete, node, false});
}

void node_actions_set_favorite(NodeNum node, bool favorite)
{
    node_actions_init();
    queue_push({NodeActionType::SetFavorite, node, favorite});
}

void node_actions_traceroute(NodeNum node)
{
    node_actions_init();
    queue_push({NodeActionType::TraceRoute, node, false});
}

void node_actions_request_user_info(NodeNum node)
{
    node_actions_init();
    queue_push({NodeActionType::RequestUserInfo, node, false});
}

uint32_t node_actions_change_tick()
{
    return s_change_tick;
}

}

#endif
