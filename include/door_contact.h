#ifndef DOOR_CONTACT_H
#define DOOR_CONTACT_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef void (*door_contact_cb_t)(bool open);

/* Configure the door switch GPIO (input, internal pull-up), install a both-edge
 * ISR + debounce one-shot timer, and register the state-change callback.
 * The callback fires once per settled transition, in esp_timer task context. */
esp_err_t door_contact_init(int gpio, uint32_t debounce_ms, bool active_low,
                            door_contact_cb_t cb);

/* Current debounced logical door-open state. */
bool door_contact_is_open(void);

#endif // DOOR_CONTACT_H
