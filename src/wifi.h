#pragma once
#include <stdbool.h>
#include <stdint.h>

bool wifi_connect(uint32_t timeout_ms);

void wifi_disconnect(void);