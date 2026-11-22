/* Host tests: product switching is scope disposal.
 *
 * The window is created hidden (wada_app_desc.hidden), so this exercises the
 * real host, app and command registry without a visible window.
 */
#include <stdio.h>
#include <string.h>

#include <cui.h>

#include "wada.h"
#include "wada_app.h"
#include "wada_log.h"
#include "wada_product.h"
#include "wada_settings.h"
#include "wada_settings_ui.h"
#include "wada_store_sqlite.h"

static int g_checks = 0;

#define CHECK(expr)                                                                     \
    do {                                                                                \
        g_checks++;                                                                     \
        if (!(expr)) {                                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);             \
            return 1;                                                                   \
        }                                                                               \
    } while (0)

static int enters_a;
static int leaves_a;
static int frames_a;
static int enters_b;
static int leaves_b;
static int frames_b;
static int invoked;
static int entered_events;
static char entered_last[32];

static void on_entered(void *user, const char *topic, const void *data, size_t len) {
    (void)user;
    (void)topic;
    entered_events++;
    if (data && len > 0) {
        size_t n = len < sizeof entered_last ? len : sizeof entered_last - 1;
        memcpy(entered_last, data, n);
        entered_last[n] = '\0';
    }
}

static void on_command(void *user) {
    (void)user;
    invoked++;
}

static void *enter_a(wada_app *app, wada_scope *scope, const char *data_dir, wada_err *err) {
    (void)data_dir;
    (void)err;
    enters_a++;
    wada_app_command(app, scope, "a.cmd", "A command", "Ctrl A", on_command, NULL);
    return &enters_a;
}

static void leave_a(void *state) {
    (void)state;
    leaves_a++;
}

static void frame_a(void *state, wada_app *app) {
    (void)state;
    (void)app;
    frames_a++;
}

static void *enter_b(wada_app *app, wada_scope *scope, const char *data_dir, wada_err *err) {
    (void)data_dir;
    (void)err;
    enters_b++;
    wada_app_command(app, scope, "b.cmd", "B command", "Ctrl B", on_command, NULL);
    return &enters_b;
}

static void leave_b(void *state) {
    (void)state;
    leaves_b++;
}

static void frame_b(void *state, wada_app *app) {
    (void)state;
    (void)app;
    frames_b++;
}

static const wada_product product_a = {"a", "A", enter_a, leave_a, frame_a};
static const wada_product product_b = {"b", "B", enter_b, leave_b, frame_b};

static void compose(wada_app *app, void *user) {
    (void)app;
    wada_host_frame((wada_host *)user);
}

/* Scripted input: the shell's step hook is called before the frame draws, so
 * these events are read by the same frame (CUI clears input at frame end). */
static void press_ctrl_b(wada_app *app, int frame, void *user) {
    (void)user;
    if (frame != 0) return;
    cui_ctx *ui = wada_app_ui(app);
    if (!ui) return;
    cui_input_key(ui, CUI_KEY_B, true, CUI_MOD_PRIMARY);
    cui_input_key(ui, CUI_KEY_B, false, CUI_MOD_PRIMARY);
}

static void press_escape(wada_app *app, int frame, void *user) {
    (void)user;
    if (frame != 0) return;
    cui_ctx *ui = wada_app_ui(app);
    if (!ui) return;
    cui_input_key(ui, CUI_KEY_ESCAPE, true, 0);
    cui_input_key(ui, CUI_KEY_ESCAPE, false, 0);
}

int main(void) {
    wada_log_set_level(WADA_LOG_WARN);
    wada_err err;
    wada_err_clear(&err);

    wada_app_desc desc;
    desc.title = "host-test";
    desc.width = 400;
    desc.height = 300;
    desc.light_theme = false;
    desc.hidden = true;

    wada_app *app = wada_app_create(&desc, &err);
    if (!app) {
        fprintf(stderr, "host tests: app create failed: %s\n", wada_err_msg(&err));
        return 1;
    }

    const wada_product *products[] = {&product_a, &product_b};
    wada_host *host = wada_host_create(app, NULL, ".", products, 2, &err);
    if (!host) {
        fprintf(stderr, "host tests: host create failed: %s\n", wada_err_msg(&err));
        wada_app_destroy(app);
        return 1;
    }

    wada_sub *sub_entered = wada_bus_subscribe_scoped(
        wada_app_bus(app), wada_app_scope(app), "product/entered", 0, on_entered, NULL);
    CHECK(sub_entered != NULL);

    /* Two host commands plus the app's own two (quit, theme) exist before any
     * product enters. */
    CHECK(wada_app_command_count(app) == 4);
    CHECK(wada_host_switch(host, "a"));
    wada_app_run_frames(app, compose, host, 2);

    CHECK(enters_a == 1);
    CHECK(leaves_a == 0);
    CHECK(frames_a >= 1);
    CHECK(entered_events == 1 && strcmp(entered_last, "a") == 0);
    CHECK(strcmp(wada_host_current(host), "a") == 0);
    CHECK(wada_app_command_count(app) == 5); /* four + a.cmd */
    CHECK(wada_app_invoke(app, "a.cmd"));
    CHECK(invoked == 1);
    CHECK(!wada_app_invoke(app, "b.cmd"));

    /* Switching disposes A's scope: its command disappears, B's appears. */
    CHECK(wada_host_switch(host, "b"));
    CHECK(!wada_host_switch(host, "missing"));
    wada_app_run_frames(app, compose, host, 2);

    CHECK(leaves_a == 1);
    CHECK(enters_b == 1);
    CHECK(frames_b >= 1);
    CHECK(entered_events == 2 && strcmp(entered_last, "b") == 0);
    CHECK(strcmp(wada_host_current(host), "b") == 0);
    CHECK(wada_app_command_count(app) == 5);
    CHECK(!wada_app_invoke(app, "a.cmd"));
    CHECK(wada_app_invoke(app, "b.cmd"));
    CHECK(invoked == 2);

    /* The settings panel is a shell surface: it adds one command, opens above
     * the product, and stops shortcuts while it is up. */
    wada_settings *prefs = wada_settings_open(wada_store_sqlite(), ":memory:", &err);
    CHECK(prefs != NULL);
    wada_settings_ui *panel = wada_settings_ui_create(app, prefs, host, ".");
    CHECK(panel != NULL);
    CHECK(wada_app_command_count(app) == 6);
    CHECK(!wada_app_surface_active(app));
    CHECK(wada_app_invoke(app, "app.settings"));
    CHECK(wada_app_surface_active(app));
    wada_app_run_frames(app, compose, host, 2);
    CHECK(wada_app_surface_active(app));
    CHECK(frames_b >= 3);

    /* A real key press reaches the frame, but the surface suppresses the
     * product shortcut: b.cmd has Ctrl+B, so feed Ctrl+B and assert it did not
     * run while the dialog is open. */
    int before = invoked;
    wada_app_run_steps(app, compose, host, 2, press_ctrl_b, NULL);
    CHECK(invoked == before);
    CHECK(wada_app_surface_active(app));

    /* Escape is the dialog's: it closes, and the same shortcut now works. */
    wada_app_run_steps(app, compose, host, 2, press_escape, NULL);
    CHECK(!wada_app_surface_active(app));
    wada_app_run_steps(app, compose, host, 2, press_ctrl_b, NULL);
    CHECK(invoked == before + 1);

    /* The panel's actions go through the same queue a click would use. */
    wada_settings_ui_set_theme(panel, true);
    wada_app_run_frames(app, compose, host, 2);
    CHECK(wada_app_is_light(app));
    char value[16];
    CHECK(wada_settings_get(prefs, "ui/theme", value, sizeof value, NULL) == WADA_OK);
    CHECK(strcmp(value, "1") == 0);

    CHECK(wada_settings_ui_set_language(panel, "vi"));
    CHECK(!wada_settings_ui_set_language(panel, "xx"));
    wada_app_run_frames(app, compose, host, 2);
    CHECK(wada_i18n_lang() == 1);
    CHECK(wada_settings_get(prefs, "ui/lang", value, sizeof value, NULL) == WADA_OK);
    CHECK(strcmp(value, "vi") == 0);
    /* The status bar is replaced by the language name, in that language, so a
     * switch cannot leave stale text behind. */
    CHECK(strcmp(wada_app_status_text(app), "Tiếng Việt") == 0);

    /* Switching product from the panel keeps the panel alive. */
    CHECK(wada_settings_ui_set_product(panel, "a"));
    wada_app_run_frames(app, compose, host, 2);
    CHECK(strcmp(wada_host_current(host), "a") == 0);
    CHECK(enters_a == 2);
    CHECK(leaves_b == 1); /* b was left once: when the panel switched away */
    CHECK(leaves_a == 1);
    CHECK(wada_app_surface_active(app) == false); /* closed by Escape earlier */

    /* A hidden window must not wait for events between frames: running more
     * frames than an animation lasts is a regression test for the frame loop
     * blocking on a window nobody can click. */
    CHECK(wada_app_run_frames(app, compose, host, 12) == 0);

    wada_settings_ui_destroy(panel);
    CHECK(wada_app_command_count(app) == 5);
    wada_settings_close(prefs);

    /* Destroying the host leaves the current product. */
    wada_host_destroy(host);
    CHECK(leaves_a == 2);
    wada_app_destroy(app);

    printf("host tests: %d checks passed\n", g_checks);
    return 0;
}
