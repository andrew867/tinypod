#ifndef TP_PATH_H
#define TP_PATH_H

#include "tp_db.h"

/*
 * Normalize iPod-style path to absolute under mount_root.
 * ipod_path examples:
 *   :iPod_Control:Music:F00:ABCD.mp3
 *   /iPod_Control/Music/F00/ABCD.mp3
 *   iPod_Control/Music/F00/ABCD.mp3
 *   F00/ABCD.mp3  (relative to Music when base provided)
 *
 * out_abs must be freed by caller on success statuses that allocate.
 */
enum tp_path_status tp_path_resolve(const char *mount_root,
                                    const char *ipod_path,
                                    char **out_abs);

/* Join mount + base_rel + rel (base like iPod_Control/Music). */
enum tp_path_status tp_path_resolve_parts(const char *mount_root,
                                          const char *base_rel,
                                          const char *rel_file,
                                          char **out_abs);

#endif
