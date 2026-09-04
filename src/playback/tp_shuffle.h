/*
 * Shuffle that spreads an artist out, rather than merely randomising.
 *
 * The caller hands over one entry per queued track and gets back a play
 * order. Nothing here knows what a track is: the module only sees the group
 * each entry belongs to, which keeps it testable and keeps the queue's own
 * representation out of it.
 */
#ifndef TP_SHUFFLE_H
#define TP_SHUFFLE_H
#include <stddef.h>
#include <stdint.h>
/*
 * One entry to be ordered. `group` is what should be spread apart - the
 * artist, or the album where there is no artist. Items with group 0 are
 * treated as ungrouped and never held apart from anything.
 */
struct tp_shuffle_item {
    uint32_t group;
};
/*
 * Fill order[0..n-1] with a permutation of 0..n-1.
 *
 * Uniform to begin with, then rearranged so that entries sharing a group
 * are spread as evenly as the set allows. Every index appears exactly
 * once - this is a permutation, always, whatever the grouping.
 */
void tp_shuffle_order(const struct tp_shuffle_item *items, size_t n,
                      size_t *order, unsigned seed);
#endif
