#ifndef TP_MOUNT_H
#define TP_MOUNT_H

#include "tp_db.h"

struct tp_volume {
    char *mount_root;          /* volume root (parent of iPod_Control) */
    char *ipod_control_root;   /* .../iPod_Control */
    enum tp_vol_health health;
};

void tp_volume_free(struct tp_volume *v);

/*
 * Resolve volume from explicit path, TINYPOD_MOUNT, known N31 paths,
 * then /proc/mounts. Never uses hardcoded developer sample paths.
 */
int tp_mount_detect(const char *cli_mount, struct tp_volume *out);

#endif
