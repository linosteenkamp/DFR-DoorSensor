#ifndef ZB_ROUTER_H
#define ZB_ROUTER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Start the Zigbee stack as a Router (radio always on). Restores network state
 * if already joined, else begins BDB steering. Returns once the stack task is
 * running; join completes asynchronously. */
esp_err_t zb_router_init(void);

/* Block until joined or the timeout elapses. Returns true if joined. */
bool zb_router_wait_ready(uint32_t timeout_ms);

/* Set the IAS Zone ZoneStatus from the current door state. Takes the Zigbee
 * lock; callable from any FreeRTOS task and before join (value is cached). */
void zb_router_set_contact(bool open);

#endif // ZB_ROUTER_H
