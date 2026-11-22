/* Hello product — the smallest complete product.
 *
 * It demonstrates the four things a product does:
 *   1. enter   — allocate state, register commands owned by the given scope,
 *                subscribe to the bus (also owned by the scope);
 *   2. frame   — draw with CUI against the app's context;
 *   3. leave   — free state; the host disposes the scope (commands and
 *                subscriptions) by itself;
 *   4. declare a wada_product table at the bottom.
 *
 * Build: cmake --build build --target wada_hello
 * Run:   build\wada_hello.exe
 */
#include <cui.h>
#include <stdlib.h>

#include "wada_app.h"
#include "wada_log.h"
#include "wada_product.h"

typedef struct hello_state {
    wada_app *app;
    int bumps;
} hello_state;

/* Bus listener: the command publishes, the product reacts. Nothing global. */
static void on_bumped(void *user, const char *topic, const void *data, size_t len) {
    (void)topic;
    (void)data;
    (void)len;
    ((hello_state *)user)->bumps++;
}

static void bump(hello_state *st) {
    wada_app_emit(st->app, "hello/bumped", NULL, 0);
    wada_app_set_status(st->app, "bumped");
}

static void cmd_bump(void *user) {
    bump((hello_state *)user);
}

static void cmd_reset(void *user) {
    hello_state *st = (hello_state *)user;
    st->bumps = 0;
    wada_app_set_status(st->app, "counter reset");
}

static void *hello_enter(wada_app *app, wada_scope *scope, const char *data_dir, wada_err *err) {
    (void)data_dir;
    (void)err;
    hello_state *st = (hello_state *)calloc(1, sizeof *st);
    if (!st) return NULL;
    st->app = app;

    /* Commands are owned by the product scope. Ctrl+B now really works: the
     * shell parses the shortcut string and dispatches it. */
    wada_app_command(app, scope, "hello.bump", "Bump the counter", "Ctrl B", cmd_bump, st);
    wada_app_command(app, scope, "hello.reset", "Reset the counter", "Ctrl R", cmd_reset, st);

    /* A subscription is a registration too, so the scope owns it. */
    wada_sub *sub = wada_bus_subscribe(wada_app_bus(app), "hello/bumped", 0, on_bumped, st);
    if (sub) wada_scope_own(scope, wada_bus_cancel_owned, sub);

    return st;
}

static void hello_leave(void *state) {
    free(state);
}

static void hello_frame(void *state, wada_app *app) {
    hello_state *st = (hello_state *)state;
    st->app = app;
    cui_ctx *ui = wada_app_ui(app);

    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER,
               .justify = CUI_JUSTIFY_CENTER, .gap = 12) {
        cui_text(ui, "Hello product", .font = CUI_FONT_DISPLAY, .size = 30,
                 .align = CUI_TEXT_CENTER);
        cui_textf(ui, "Bumped %d time%s", st->bumps, st->bumps == 1 ? "" : "s");
        if (cui_button(ui, "Bump", .variant = CUI_BUTTON_PRIMARY, .width = CUI_PX(160))) bump(st);
        if (cui_button(ui, "Reset", .variant = CUI_BUTTON_OUTLINE, .width = CUI_PX(160))) cmd_reset(st);
        cui_text(ui, "Ctrl+B bumps, Ctrl+R resets, Ctrl+K opens every command.",
                 .size = 12, .color = CUI_COLOR_TEXT_MUTED, .align = CUI_TEXT_CENTER);
    }
}

const wada_product hello_product = {
    "hello",
    "Hello",
    hello_enter,
    hello_leave,
    hello_frame,
};
