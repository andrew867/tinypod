#ifndef TP_FILE_PROBE_H
#define TP_FILE_PROBE_H

#include "tp_db.h"

enum tp_codec tp_file_probe_codec(const char *path);
int tp_file_probe_playable(const char *path, enum tp_codec *out_codec);

#endif
