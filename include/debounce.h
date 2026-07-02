#ifndef DEBOUNCE_H
#define DEBOUNCE_H

#include <stdbool.h>

/* Debounce settling: the ISR restarts a one-shot timer on every edge; when the
 * timer finally expires the settled GPIO level is passed to debounce_settle().
 * This module decides whether that settled level is a real transition. */
typedef struct {
    int stable_level;
} debounce_t;

void debounce_init(debounce_t *d, int initial_level);

/* Returns true (and updates stable_level) only when sampled_level differs from
 * the current stable level. Returns false when the level is unchanged. */
bool debounce_settle(debounce_t *d, int sampled_level);

int debounce_stable_level(const debounce_t *d);

#endif // DEBOUNCE_H
