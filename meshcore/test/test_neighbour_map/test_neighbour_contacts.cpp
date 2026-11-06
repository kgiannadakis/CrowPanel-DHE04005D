#include "../../examples/crowpanel_p4_lvgl_chat/neighbour_contact_policy.h"
using namespace NeighbourContactCleanup;

constexpr uint8_t key[32] = {0x55, 0x68, 0x7C, 0x00};
constexpr const char* placeholder = "Repeater_55687C00";
static_assert(isPlaceholderName(placeholder, key));
static_assert(!isPlaceholderName("", key));
static_assert(!isPlaceholderName("Repeater_5568", key), "Keep discovery fallback names");
static_assert(!isPlaceholderName("Repeater_55687C01", key), "Require the contact's own key");
static_assert(!isPlaceholderName("Repeater_55687C00 extra", key));
static_assert(!isPlaceholderName("Repeater_55687c00", key));
static_assert(!isPlaceholderName(nullptr, key));
static_assert(!isPlaceholderName(placeholder, nullptr));
static_assert(!isPlaceholderName("KaA_RK#1_Deurne", key));

static_assert(shouldRemove(placeholder, key, true, 0, 0, 0, 0, false));
static_assert(!shouldRemove(placeholder, key, false, 0, 0, 0, 0, false), "Keep companions");
static_assert(!shouldRemove(placeholder, key, true, 1, 0, 0, 0, false), "Keep advert-backed repeaters");
static_assert(!shouldRemove(placeholder, key, true, 0, 51000000, 0, 0, false), "Keep saved latitude");
static_assert(!shouldRemove(placeholder, key, true, 0, 0, 4400000, 0, false), "Keep saved longitude");
static_assert(!shouldRemove(placeholder, key, true, 0, 0, 0, 1, false), "Keep favourites/flags");
static_assert(!shouldRemove(placeholder, key, true, 0, 0, 0, 0, true), "Keep learned routes");
static_assert(!shouldRemove("My Repeater", key, true, 0, 0, 0, 0, false));
