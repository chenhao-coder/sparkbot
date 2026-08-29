#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Connect to the configured WiFi AP in station mode.
 * Blocks until an IP address is obtained (or retries are exhausted).
 */
void oai_wifi(void);

#ifdef __cplusplus
}
#endif
