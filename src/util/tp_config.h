#ifndef TP_CONFIG_H
#define TP_CONFIG_H

#include <stdint.h>

struct tp_config {
    char *last_mount;
    uint64_t last_track_id;
    uint32_t last_position_ms;
    int shuffle;
    char repeat[16]; /* off|one|all */
    /* An alsa-lib device name from /etc/asound.conf, or empty for the
       default. Stored by name rather than as an index so that adding a
       device to asound.conf cannot silently renumber somebody's choice. */
    char output[24];
};

void tp_config_init(struct tp_config *c);
void tp_config_free(struct tp_config *c);
int tp_config_load(struct tp_config *c);
int tp_config_save(const struct tp_config *c);

#endif
