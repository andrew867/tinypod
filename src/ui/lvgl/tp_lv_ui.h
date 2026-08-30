/*
 * tp_lv_ui.h — TinyPod's graphical UI.
 *
 * 240 x 432, no touchscreen, three usable buttons. See tp_lv_input.h for what
 * they do and why HOME is not one of them.
 *
 * This sits entirely on top of tp_app: it reads the library and calls the same
 * play/pause/next/prev commands the CLI does, and owns no music state of its
 * own. The ANSI UI in tp_ui_fb.c is unaffected and still reachable as `ui`.
 */

#ifndef TP_LV_UI_H
#define TP_LV_UI_H

struct tp_app;

/*
 * Run until the user leaves, or forever - on the device the launcher ends this
 * process with SIGTERM when HOME is pressed, which is the normal way out.
 *
 * `fb` is the framebuffer device, or NULL for /dev/fb0.
 */
int tp_lv_ui_run(struct tp_app *app, const char *fb);

/* Render a walk through the real UI, against a real library, to BMPs in
   `dir`. Same draw() and on_key() as the device; no framebuffer needed. */
int tp_lv_ui_shots(struct tp_app *app, const char *dir);

#endif
