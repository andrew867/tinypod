#include "tp_ui_model.h"

void tp_ui_model_init(struct tp_ui_model *m)
{
    m->screen = TP_UI_HOME;
    m->selection = 0;
}

int tp_ui_model_max_sel(struct tp_ui_model *m, struct tp_app *app)
{
    switch (m->screen) {
    case TP_UI_HOME:
        return 7;
    case TP_UI_SONGS:
        return app->lib.track_count ? (int)app->lib.track_count - 1 : 0;
    case TP_UI_ARTISTS:
        return app->lib.artist_count ? (int)app->lib.artist_count - 1 : 0;
    case TP_UI_ALBUMS:
        return app->lib.album_count ? (int)app->lib.album_count - 1 : 0;
    case TP_UI_PLAYLISTS:
        return app->lib.playlist_count ? (int)app->lib.playlist_count - 1 : 0;
    default:
        return 0;
    }
}
