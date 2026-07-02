#ifndef CONTACT_MAP_H
#define CONTACT_MAP_H

#include <stdbool.h>

/* Map a raw GPIO level to logical door-open.
 * active_low == true : a LOW level (0) means the door is OPEN.
 * active_low == false: a HIGH level (nonzero) means the door is OPEN. */
bool contact_is_open(int gpio_level, bool active_low);

#endif // CONTACT_MAP_H
