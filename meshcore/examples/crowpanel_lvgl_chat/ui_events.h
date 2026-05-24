// ui_events.h — Navigation event stubs (tab-bar replaces SquareLine nav)

#ifndef _UI_EVENTS_H
#define _UI_EVENTS_H

#ifdef __cplusplus
extern "C" {
#endif

// These were SquareLine navigation callbacks.
// Now unused — tab bar handles all screen transitions.
// Kept as stubs so any leftover references still link.
void ui_event_homebutton2(lv_event_t * e);
void ui_event_homebutton3(lv_event_t * e);
void ui_event_settingsbutton1(lv_event_t * e);
void ui_event_settingsbutton2(lv_event_t * e);
void ui_event_backbutton1(lv_event_t * e);
void ui_event_backbutton2(lv_event_t * e);
void ui_event_Button2(lv_event_t * e);
void ui_event_Button3(lv_event_t * e);
void ui_event_repeatersbutton(lv_event_t * e);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif
