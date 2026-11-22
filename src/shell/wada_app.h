/* Wada app shell.
 *
 * The shell owns what CUI deliberately leaves to the application: the app
 * object, the root scope, the event bus, the service registry, commands, the
 * command palette, the status bar and the frame loop. It knows no domain: the
 * product supplies one compose callback and registers commands.
 */
#ifndef WADA_APP_H
#define WADA_APP_H

#include "wada.h"
#include "wada_log.h"

typedef struct cui_ctx cui_ctx;
typedef struct wada_app wada_app;

typedef struct wada_app_desc {
    const char *title; /* static, owned by the caller */
    int width;
    int height;
    bool light_theme;
    bool hidden; /* no visible window (tests, --doctor) */
} wada_app_desc;

typedef struct wada_command_info {
    const char *id;
    const char *title;
    const char *shortcut; /* may be NULL */
} wada_command_info;

typedef void (*wada_command_fn)(void *user);
typedef void (*wada_compose_fn)(wada_app *app, void *user);
typedef void (*wada_defer_fn)(void *user);

/* A shell surface draws above the product (a modal panel, a wizard). It is
 * registered by the shell itself, never by a product: the host owns the frame.
 * While *open is true, shortcuts and the palette are not dispatched, so a
 * panel's own widgets keep the keyboard. */
typedef void (*wada_surface_fn)(wada_app *app, void *user);

/* A step runs at the start of every frame, after deferred work and before the
 * frame draws. It is the injection point for tests: the drawing context is
 * valid, so step functions may feed cui_input_* and drive real widgets and
 * shortcuts. `frame` counts from 0 within the run. */
typedef void (*wada_step_fn)(wada_app *app, int frame, void *user);

WADA_API wada_app *wada_app_create(const wada_app_desc *desc, wada_err *err);
WADA_API void wada_app_destroy(wada_app *app);

WADA_API wada_scope *wada_app_scope(wada_app *app);
WADA_API wada_bus *wada_app_bus(wada_app *app);
WADA_API wada_registry *wada_app_registry(wada_app *app);

/* The drawing context, valid only inside a compose callback. */
WADA_API cui_ctx *wada_app_ui(wada_app *app);

/* Commands are owned by the scope passed in; destroying it removes them. */
WADA_API wada_status wada_app_command(wada_app *app, wada_scope *owner, const char *id, const char *title,
                                      const char *shortcut, wada_command_fn fn, void *user);
WADA_API bool wada_app_invoke(wada_app *app, const char *id);
WADA_API int wada_app_command_count(wada_app *app);
/* Live commands, for doctor output and tests. */
WADA_API bool wada_app_command_at(wada_app *app, int index, wada_command_info *out);

/* Deferred work runs at the start of the next frame, never inside a widget
 * callback: safe place for open/close/rebuild operations. */
WADA_API wada_status wada_app_defer(wada_app *app, wada_defer_fn fn, void *user);

WADA_API void wada_app_emit(wada_app *app, const char *topic, const void *data, size_t len);
WADA_API void wada_app_set_status(wada_app *app, const char *text);
WADA_API const char *wada_app_status_text(wada_app *app);
WADA_API void wada_app_set_title(wada_app *app, const char *title);
WADA_API void wada_app_size(wada_app *app, int *width, int *height);
WADA_API bool wada_app_is_light(wada_app *app);
WADA_API void wada_app_set_light(wada_app *app, bool light);
WADA_API void wada_app_toggle_theme(wada_app *app);
WADA_API void wada_app_quit(wada_app *app);

/* Draws `fn` after the product and the status bar. `open` must stay alive for
 * as long as the app; the surface may be replaced at any time. */
WADA_API void wada_app_set_surface(wada_app *app, wada_surface_fn fn, void *user, const bool *open);
WADA_API bool wada_app_surface_active(wada_app *app);

/* Blocks until the window closes. Returns 0 normally. */
WADA_API int wada_app_run(wada_app *app, wada_compose_fn compose, void *user);

/* Runs at most frames frames (for tests and --doctor), then closes. */
WADA_API int wada_app_run_frames(wada_app *app, wada_compose_fn compose, void *user, int frames);

/* Same, with `step` called before each frame draws (input injection for tests).
 * Either may be NULL. */
WADA_API int wada_app_run_steps(wada_app *app, wada_compose_fn compose, void *user, int frames,
                                wada_step_fn step, void *step_user);

#endif /* WADA_APP_H */
