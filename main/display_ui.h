#pragma once

#include "esp_err.h"
#include "app_state.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t display_ui_init(void);
void display_ui_update(const app_status_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif

