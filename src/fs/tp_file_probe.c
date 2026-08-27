#include "tp_file_probe.h"
#include "tp_util.h"

#include <stdio.h>
#include <string.h>

static enum tp_codec from_ext(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot || !dot[1])
        return TP_CODEC_UNKNOWN;
    if (tp_strcasecmp(dot, ".mp3") == 0)
        return TP_CODEC_MP3;
    if (tp_strcasecmp(dot, ".m4a") == 0 || tp_strcasecmp(dot, ".aac") == 0 ||
        tp_strcasecmp(dot, ".mp4") == 0 || tp_strcasecmp(dot, ".m4b") == 0)
        return TP_CODEC_AAC;
    if (tp_strcasecmp(dot, ".m4p") == 0)
        return TP_CODEC_PROTECTED_UNSUPPORTED;
    if (tp_strcasecmp(dot, ".wav") == 0)
        return TP_CODEC_WAV;
    if (tp_strcasecmp(dot, ".aiff") == 0 || tp_strcasecmp(dot, ".aif") == 0)
        return TP_CODEC_AIFF;
    if (tp_strcasecmp(dot, ".alac") == 0)
        return TP_CODEC_ALAC;
    return TP_CODEC_UNKNOWN;
}

enum tp_codec tp_file_probe_codec(const char *path)
{
    unsigned char hdr[16];
    FILE *f;
    size_t n;
    enum tp_codec by_ext;

    if (!path)
        return TP_CODEC_UNKNOWN;
    by_ext = from_ext(path);

    f = fopen(path, "rb");
    if (!f)
        return by_ext;
    n = fread(hdr, 1, sizeof(hdr), f);
    fclose(f);
    if (n < 4)
        return by_ext;

    /* ID3 or MPEG frame sync */
    if (hdr[0] == 'I' && hdr[1] == 'D' && hdr[2] == '3')
        return TP_CODEC_MP3;
    if (hdr[0] == 0xFF && (hdr[1] & 0xE0) == 0xE0)
        return TP_CODEC_MP3;

    /* MP4/M4A ftyp */
    if (n >= 12 && hdr[4] == 'f' && hdr[5] == 't' && hdr[6] == 'y' && hdr[7] == 'p') {
        /* Check for drm brands */
        if (memcmp(hdr + 8, "M4P ", 4) == 0 || memcmp(hdr + 8, "drmi", 4) == 0)
            return TP_CODEC_PROTECTED_UNSUPPORTED;
        if (memcmp(hdr + 8, "alac", 4) == 0)
            return TP_CODEC_ALAC;
        return TP_CODEC_AAC;
    }

    if (memcmp(hdr, "RIFF", 4) == 0)
        return TP_CODEC_WAV;
    if (memcmp(hdr, "FORM", 4) == 0)
        return TP_CODEC_AIFF;

    return by_ext;
}

int tp_file_probe_playable(const char *path, enum tp_codec *out_codec)
{
    enum tp_codec c = tp_file_probe_codec(path);
    if (out_codec)
        *out_codec = c;
    if (!tp_is_readable_file(path))
        return 0;
    if (c == TP_CODEC_PROTECTED_UNSUPPORTED || c == TP_CODEC_UNKNOWN)
        return 0;
    return 1;
}
