# Examples

Each folder is a complete product on the Wada core, deliberately small enough
to read in one sitting:

| Example | Files | Shows |
|---|---|---|
| [`hello/`](hello) | 2 | product lifecycle, commands, shortcuts, the event bus, scopes |
| [`todo/`](todo) | 2 | persistence through the `store` service, a versioned record format, toggle/clear |

The shipped app's products in [`../src/apps/`](../src/apps) are the fuller
references (notes, learning); these examples are the teaching versions.

## Build and run

From the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
.\build\wada_hello.exe
.\build\wada_todo.exe
```

Each example passes a `data_subdir` to boot (`Wada Hello`, `Wada Todo`), so it
reads and writes its own database under the app data root and cannot touch the
shipped app's notes. Only one Wada process can hold a data directory at a time
(`wada.lock`), so run the examples one at a time or point `WADA_DATA_DIR` at
scratch folders.

## The product contract

A product is a `wada_product` table:

```c
typedef struct wada_product {
    const char *id;      /* "hello" — used in command ids and settings */
    const char *title;   /* "Hello" — window title while active */
    void *(*enter)(wada_app *app, wada_scope *scope, const char *data_dir, wada_err *err);
    void (*leave)(void *state);
    void (*frame)(void *state, wada_app *app);
    const wada_i18n_pair *strings;  /* optional: static translation table */
    int string_count;
    const char *const *walk;        /* optional: commands --doctor invokes to
                                       reach states one frame does not draw */
    int walk_count;
} wada_product;
```

- **enter** allocates state and registers everything it owns through `scope`:
  `wada_app_command(app, scope, ...)`, `wada_bus_subscribe` + `wada_scope_own`,
  services. It returns the state pointer.
- **frame** draws with CUI. It must not block, and it must not destroy widgets
  from inside their handlers — use `wada_app_defer` for that.
- **leave** frees the state. It does **not** unregister commands or
  subscriptions: the host destroys the scope, which disposes them in reverse
  order.
- An app is a `main` with a product table and one call:

```c
int main(int argc, char **argv) {
    const wada_product *products[] = { &hello_product };
    wada_boot_desc desc = { "Hello", 900, 620, false, wada_store_sqlite(),
                            products, 1, "hello" };
    return wada_boot_run(&desc, argc, argv);
}
```

The examples are English-only: they pass no `strings` table, so the shell's
strings are translated but their own labels stay English. A real product adds a
static `{English, translated}` table and points the product at it; the host
registers it when the app starts (the panel and palette need every product's
title while another product is active). Draw every user-visible string through
`wada_tr` and formatted ones through `wada_trf`; `--doctor` then reports how many
drawn strings still lack a translation.

`walk` is how a product lets `--doctor` see its other screens: ids in that array
are invoked one per frame against a throwaway database, so the strings of a modal
state or a confirmation are counted too. List only commands that are safe there
— never import/export or anything that opens a dialog.

## Adding a product to the shipped app

1. Create `src/apps/<name>/` with the repo logic and `<name>_product.c`.
2. Add both `.c` files to the `wada` executable in `CMakeLists.txt`.
3. Declare the product in `src/main.c` and add it to the array.
4. Nothing else: the product host, palette, settings and store service are
   already there.

If a product needs storage, ask the registry for the `store` service instead of
linking the provider:

```c
const wada_store_ops *ops =
    wada_registry_get(wada_app_registry(app), "store", WADA_STORE_ABI, NULL);
```

Keys are namespaced (`"todo/…"`, `"note/…"`, `"settings/…"`); values are yours
to encode, with a `v1` line so a later format can be detected.
