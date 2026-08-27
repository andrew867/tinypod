#ifndef TP_UI_MODEL_H
#define TP_UI_MODEL_H

#include "tp_app.h"

enum tp_ui_screen {
    TP_UI_HOME = 0,
    TP_UI_SONGS,
    TP_UI_ARTISTS,
    TP_UI_ALBUMS,
    TP_UI_PLAYLISTS,
    TP_UI_NOW,
    TP_UI_SETTINGS,
    TP_UI_ABOUT
};

struct tp_ui_model {
    enum tp_ui_screen screen;
    int selection;
};

void tp_ui_model_init(struct tp_ui_model *m);
int tp_ui_model_max_sel(struct tp_ui_model *m, struct tp_app *app);

#endif
