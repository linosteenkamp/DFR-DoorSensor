#include "debounce.h"

void debounce_init(debounce_t *d, int initial_level)
{
    d->stable_level = initial_level;
}

bool debounce_settle(debounce_t *d, int sampled_level)
{
    if (sampled_level == d->stable_level) {
        return false;
    }
    d->stable_level = sampled_level;
    return true;
}

int debounce_stable_level(const debounce_t *d)
{
    return d->stable_level;
}
