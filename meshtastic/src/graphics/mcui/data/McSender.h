#pragma once
#if HAS_TFT && USE_MCUI

#include "McMessages.h"
#include <cstdint>

namespace mcui {

bool sender_send_text(const McConvId &id, const char *text);

}

#endif
