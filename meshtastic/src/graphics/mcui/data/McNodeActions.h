#pragma once
#if HAS_TFT && USE_MCUI

#include "mesh/NodeDB.h"
#include <cstdint>

namespace mcui {

void node_actions_init();
void node_actions_clear_all();
void node_actions_delete(NodeNum node);
void node_actions_set_favorite(NodeNum node, bool favorite);
void node_actions_traceroute(NodeNum node);
uint32_t node_actions_change_tick();

}

#endif
