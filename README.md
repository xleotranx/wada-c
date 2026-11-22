# Wada

A native C99 app core: event loop, shell, scopes, services and versioned
interfaces. Products — notes, learning, whatever comes next — are built on top
by registering commands, subscribing to the bus and implementing provider
interfaces. Nothing in the core knows what the product is.

```
src/core/             wada.h  err  log  scope  event  registry  iface
src/shell/            wada_app (window, frame loop, commands, palette, status,
                                 surfaces)
                      wada_host (product registry + switching)
                      wada_settings (key/value settings over the store)
                      wada_settings_ui (the settings panel)
                      wada_i18n (string tables: English + Tiếng Việt)
                      wada_instance (one process per data directory)
                      wada_module (provider modules: LoadLibrary/dlopen once)
                      wada_boot (shared boot: data dir, provider, app, host)
src/core/             ... plus wada_id (unique, sortable record ids)
src/providers/sqlite/ wada_store_ops over SQLite 3.54.1 (the only sqlite3.h),
                      linked into the app and built again as a loadable module
src/apps/notes/       notes: list, search, editor, autosave, export/import
src/apps/learning/    learning: cards, due queue, review session
examples/hello/       smallest complete product (lifecycle, commands, bus)
examples/todo/        store-backed product (records, toggle, clear)
tests/                core, store, notes, cards, host, i18n, module, instance
                      (377 checks)
vendor/cui/           CUI 1.4.0 (MIT), vendored source, compiled in
vendor/sqlite/        SQLite 3.54.1 amalgamation (public domain), compiled in
```

## Rules

1. `wada_core` compiles alone: no third-party header, no UI. The build
   enforces the dependency direction.
2. Every registration returns a token owned by a scope. Destroying the scope
   disposes children and entries in reverse order, so unload is mechanical.
3. Events are published over a snapshot: subscribing, cancelling or publishing
   during dispatch is defined, not undefined.
4. Provider interfaces start with `{size, version}` and are append-only.
   Storage, models and anything else pluggable implement a vtable; the core
   never includes the vendor's header. Products look the provider up as the
   `store` service instead of linking it.
5. The UI thread never blocks. Work is deferred to frame boundaries, and a
   product switch applies at a frame start, never inside a widget handler.
6. Errors are values (`wada_status` + `wada_err`), never `exit()` in a library.
7. A provider can arrive as a shared module, but it is loaded once, validated
   before any pointer is read, and never unmapped while the process runs.
8. User-visible text goes through `wada_tr`; a product's table is static data
   registered with its scope. The English text is the key.
9. One process per data directory (`wada.lock`), and a record id is unique
   across restarts: `<seconds>-<serial>-<seed>`, checked against the store.
10. The provider snapshots the database before any schema migration and once a
    day in `backups/`; a failed pre-migration snapshot aborts the open.

## Settings

`Ctrl+,` or the command palette opens the settings panel: theme, language
(`English` / `Tiếng Việt`), the active product, and the data directory with the
database size. It is a shell surface above the current product, so it stays open
while you switch product; theme and language apply at the next frame start and
are persisted immediately.

Settings live in the same database as notes and cards (`settings/…` keys), so
copying `wada.db` carries the language and theme too.

## Products

`wada_host` owns one scope per product. Switching leaves the current product,
disposes its scope — which removes every command and subscription it registered
— and enters the next product with a fresh scope. Settings persist the current
product, window size and theme.

- **Notes** — list, search, title/body editor, debounced autosave, delete,
  export/import.
- **Learning** — cards with an SM-2-style scheduler, a due queue, a review
  session (Again / Hard / Good / Easy), and an add-card form.

Switch from the command palette (`Ctrl+K` → *Notes* / *Learn*), from the
settings panel, or start in one with `wada.exe --app notes|learning`.

## Footprint

Measured on Windows 11, RelWithDebInfo build, both products linked:

| | |
|---|---|
| `wada.exe` stripped | **2.81 MB** (CUI + SQLite 3.54.1 + two products + i18n) |
| `wada_store_plugin.dll` | 6.32 MB unstripped (SQLite compiled a second time) |
| idle memory | ~41 MB private, ~96 MB working set (CUI's OpenGL driver is most of it) |
| idle CPU | ~0.17 s over the first 4 s, then near zero |

The database is one file: `<LOCALAPPDATA>\Wada\wada.db` (WAL mode). Copy it and
your notes, cards, language and theme move with it.

## Build and run

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
.\build\wada.exe               # the shipped app (Notes + Learn)
.\build\wada_hello.exe         # smallest example product
.\build\wada_todo.exe          # store-backed example product
.\build\wada.exe --doctor      # writes <data-dir>\doctor.txt
```

Flags and environment hooks:

| | |
|---|---|
| `--app notes\|learning` | start in a product |
| `--settings` | open the settings panel on start |
| `--lang en\|vi` | run in a language (also `WADA_LANG`) |
| `--provider <dll>` | load the store from a module instead of the linked one (also `WADA_STORE_MODULE`) |
| `WADA_DATA_DIR` | data folder (default `%LOCALAPPDATA%\Wada`; examples use `Wada Hello`, `Wada Todo`) |
| `WADA_STUDY=1` | learning opens in a review session, for captures |
| `WADA_NO_DAILY_BACKUP=1` | skip the once-a-day snapshot (automation; the pre-migration snapshot is never skipped) |
| `CUI_SCREENSHOT`, `CUI_SCREENSHOT_FRAMES` | CUI's PNG capture (used for `shots/`) |

Try the module path against the linked one — both write the same database:

```powershell
.\build\wada.exe --provider wada_store_plugin.dll --doctor
.\build\wada.exe --provider wada_bad_plugin.dll --doctor   # exit 1, ABI refused in the log
```

An example is a product plus a `main` with one `wada_boot_run` call; see
[`examples/README.md`](examples/README.md) for the product contract and how to
add a product to the shipped app.

Toolchain: the portable w64devkit (gcc, cmake, ninja) at `.tools/w64devkit`,
or `WADA_W64DEVKIT` pointing at its `bin`.

## Status

- Core: errors, logging, scopes, event bus, service registry, provider
  interface pattern — tested.
- Shell: window loop, commands, palette, status bar, theme, deferred work,
  product host, surfaces (the settings panel), settings persistence.
- Storage: SQLite behind `wada_store_ops` ABI 3 (open, transactions,
  get/put/delete, prefix iteration, full-text search, schema version and
  migration, last backup path), published as the `store` service. The same
  provider is also built as a loadable module with a versioned handshake
  (`wada_plugin_entry`); the loader refuses a wrong ABI without touching the
  vtable.
- Data safety: a second instance exits with code 2 (`wada.lock`); record ids are
  unique across restarts and processes; the provider writes
  `backups/wada-daily.db` once a day and `backups/wada-pre-vN-<stamp>.db`
  before a migration, and `--doctor` prints the path of the last backup.
- Products: notes and learning, each in its own scope, both on the same store
  contract.
- Notes: metadata and body are separate records; the list loads titles only,
  bodies load on selection and grow on demand, search goes through FTS5, rows
  are virtualized, delete asks for a second click, and Export/Import move every
  note through a length-prefixed backup file.
- Languages: English and Tiếng Việt. Product tables are registered by the host;
  command titles are translated at draw time; `--doctor` reports how many drawn
  strings had no translation. A system font is loaded as a CUI fallback because
  the bundled fonts are Latin-only.
- Doctor: provider source (linked or module path), plugin and store ABI,
  capabilities, schema version, language, settings, a real write/read/delete
  probe, per-prefix record counts, the command list with shortcuts, and the
  untranslated count with the number of screens drawn. Products declare the
  commands (`wada_product.walk`) that reach screens a single frame does not show,
  so a review session, an add form and a delete confirmation are walked too.
- Logging: lines go to `<data-dir>\log.txt` (the GUI build has no console), and
  a failed provider load is reported there with the reason.
- Tests: a hidden-window host test presses real keys through the frame loop
  (`wada_app_run_steps`): a product shortcut is suppressed while the settings
  panel is open, Escape closes it, and the panel's theme/language/product
  actions apply at the next frame and persist. i18n and module tests cover
  lookup/fallback/disposal and the module handshake.
- Next: the records/ui kit that removes duplication across products
  (see `docs/CORE.md` §12).
