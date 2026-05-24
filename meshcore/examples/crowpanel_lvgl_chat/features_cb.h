#pragma once
// features_cb.h — Callbacks for Features screen

#ifdef __cplusplus
extern "C" {
#endif

void features_register_callbacks();
void features_update_status_labels();
void features_field_focus(lv_obj_t* field);
void features_field_defocus(lv_obj_t* field);

#ifdef __cplusplus
}
#endif
