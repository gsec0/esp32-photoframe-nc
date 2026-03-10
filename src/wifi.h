#pragma once
#include <stdbool.h>
#include <stdint.h>

void wifi_init_once(void);

bool wifi_connect(uint32_t timeout_ms);

void wifi_disconnect(void);