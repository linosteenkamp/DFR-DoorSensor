#ifndef IAS_STATUS_H
#define IAS_STATUS_H

#include <stdint.h>
#include <stdbool.h>

/* IAS Zone ZoneStatus attribute (0x0002) bitmap. */
#define IAS_ZONE_STATUS_ALARM1 ((uint16_t)0x0001)

/* Encode logical door state into the ZoneStatus bitmap.
 * open == true  -> alarm1 bit set (contact open)
 * open == false -> 0 (contact closed). */
uint16_t ias_zone_status_encode(bool open);

#endif // IAS_STATUS_H
