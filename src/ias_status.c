#include "ias_status.h"

uint16_t ias_zone_status_encode(bool open)
{
    return open ? IAS_ZONE_STATUS_ALARM1 : (uint16_t)0;
}
