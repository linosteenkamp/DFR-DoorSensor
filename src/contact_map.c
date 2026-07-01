#include "contact_map.h"

bool contact_is_open(int gpio_level, bool active_low)
{
    bool high = (gpio_level != 0);
    return active_low ? !high : high;
}
