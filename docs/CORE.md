# Wada core — design and research record

Wada is a microkernel app core in C99. The core owns the frame loop, the shell,
scopes, an event bus and a service registry. Everything else — storage, models,
product features — is implemented behind versioned interfaces and registered
with the core. A product is a compose callback plus registered commands; the
core never learns what the product is.

This document records the research that shaped it and the rules the code
enforces.

---

## 1. What the core is, and is not

**Is:** window lifecycle, frame loop, wakeups, commands and palette, status
bar, shell surfaces (the settings panel), theme, scopes, event bus, service
registry, logging, error values, the `{size, version}` interface convention and
the plugin handshake, deferred work at frame boundaries.

**Is not:** a product, a widget toolkit (CUI does that), a database, a network
stack, an agent loop, a plugin marketplace, a policy engine, or a translation
engine (the string tables are plain data in the shell and in each product).

The line is deliberate. A core that tries to be a platform for an unknown
product becomes a framework that fits nothing. The test of this core is a
second product built on it with no core changes.

---

## 2. What sixteen systems taught us

| System | Mechanism we studied | What we took |
|---|---|---|
| **VS Code** | Declarative `contributes` + lazy `activationEvents`; extensions in a separate host process | Contribute before activating; one activation per id; never at start |
| **Obsidian** | `register*` returns handles; `onload`/`onunload`; no permissions | Registration tokens, host-enforced; but *reject* ambient trust |
| **Raycast** | Plugins return declarative UI; the host renders it and owns theming | Host-owned rendering; no plugin pixels |
| **Chrome MV3** | Manifest permissions, restartable service worker, isolated worlds | Declared capabilities; reconstruct from state, not long-lived memory |
| **JetBrains** | Extension points as interface (vtable) or bean (data); lazy services; dynamic unload only if every EP is dynamic | Two EP shapes; lazy singletons; no hot-unload promise |
| **Eclipse / OSGi** | Extension registry; INSTALLED→ACTIVE lifecycle; service ranking; lazy activation | Registry keyed by name; version ranges; stop = unregister |
| **Emacs** | Hooks are lists with depth; advice; best-effort `unload-feature`; no barriers | Integer priority + stable tie-break; barrier per callback |
| **Neovim** | `api_level`, additive-only API, `:checkhealth`, remote plugins spawn lazily | ABI constant; health entry point; lazy host spawn |
| **SQLite** | `sqlite3_module.iVersion` append-only slots; loadable extensions through a function-table API | `{size, version}` header on every vtable |
| **Qt** | IID strings; metadata read before load; unload refused while in use | Identity by name+version; metadata before code |
| **COM** | `QueryInterface` for optional interfaces; refcount per pointer | Optional-interface query instead of one giant vtable |
| **Vulkan / kernel** | `sType`/`pNext`, `apiVersion`; `size` field with zero-filled tail | Size-checked structs, append-only evolution |
| **WASM / Extism / LSP** | No shared address space; host copies bytes; trap becomes error | Keep the vtable pointer-free so an IPC/WASM adapter can implement it |
| **Home Assistant** | Manifest with hard vs soft deps; setup/retry/unload states; unique ids; listeners return removers | Hard dep fails the plugin, soft dep only orders; explicit states |
| **WordPress** | Actions vs filters; priority then registration order; activation once | Two event kinds (publish / transform later); documented ordering |
| **Cordis / Koishi** (the DSH plugin layer) | Services on a context; `inject`; typed dispatch (emit/waterfall/parallel/bail); every registration a reversible effect | Scope-owned effects; weakest extension point that suffices |
| **Django** | Three startup phases; `ready()` idempotent; no unload | Deterministic startup order; restart to change |

Two conclusions dominated the design:

1. **Declarative contributions beat imperative registration at load time.** The
   shell can render commands, menus and views from data without running
   product code.
2. **Every mature system eventually admits it cannot hot-unload safely.** Even
   JetBrains falls back to restart prompts. For C this is non-negotiable: a
   loaded module is never unloaded while callbacks may exist. Load once,
   restart to change.

---

## 3. The shape

```
┌──────────────────────────── app (one product) ────────────────────────────┐
│  compose(app, user)         registers commands, subscriptions, services    │
└───────────────▲───────────────────────────────────────────▲───────────────┘
                │                                           │
┌───────────────┴──────────── shell ─────────────────────────┴───────────────┐
│  frame loop · commands · palette · status bar · theme · deferred queue     │
│  settings panel (a surface above the product) · string tables (i18n)       │
└───────────────▲───────────────────────────────────────────▲───────────────┘
                │                                           │
┌───────────────┴──────────── core ──────────────────────────┴───────────────┐
│  err · log · scope (tokens) · bus (snapshot dispatch) · registry · iface    │
└────────────────────────────────────────────────────────────────────────────┘
                ▲
┌───────────────┴───────── providers / plugins ──────────────────────────────┐
│  sqlite store, linked or loaded as a module through one exported symbol     │
│  implement {size, version} vtables, publish them through the registry      │
└────────────────────────────────────────────────────────────────────────────┘
```

Dependencies point down. `wada_core` is a static library that does not include
`cui.h`, any product header, or any vendor header; the build enforces the
boundary.

---

## 4. Interfaces

Every provider vtable begins with:

```c
typedef struct wada_ops_header {
    uint32_t size;    /* sizeof(the concrete ops struct) */
    uint32_t version; /* ABI the provider implements */
} wada_ops_header;

bool wada_ops_ok(const wada_ops_header *hdr, size_t need, uint32_t abi);
```

Rules that come from the research:

- **Append-only.** New slots go at the end; `version` bumps only on an
  incompatible change. A caller checks `size >= needed` and refuses unknown
  trailing bytes it cannot understand.
- **Opaque handles.** `wada_store`, `wada_txn`, `wada_iter` are incomplete
  types. The host never dereferences provider memory.
- **Borrowed slices.** `get`/`next` return slices valid until the next call on
  that handle. Nothing is allocated by one runtime and freed by another — on
  Windows, two CRT heaps make that a crash.
- **Provider-owned errors** are strings inside `wada_err`; the host never frees
  provider memory.
- **No callbacks into the provider while it is executing** a call: the vtable
  is a request/response surface, not a notification channel. Events are the
  bus's job.
- **Optional capabilities** are queried (`caps()`), not assumed.

ABI 2 exercised the first real change: `search` and `schema_version` were
appended to the store table, the version moved to 2, and nothing else in the
contract moved. ABI 3 appended `last_backup` the same way: a provider now says
where its safety copy is, and a provider that has none returns `""`. Two
append-only revisions, two working callers, no slot reordered.

Search is a **structured operation on the interface**, not a query language:
`search(txn, prefix, plain_text_query, limit)`, terms ANDed, the last term a
prefix, results best-first. A provider without an index returns
`WADA_ERR_UNSUPPORTED`, and the product falls back to its own scan. The SQLite
provider implements it with FTS5 over the values; the caller never sees a
MATCH expression.

The provider owns its schema: `PRAGMA user_version` records it,
`schema_version()` reports it, and opening an older database migrates it
(rebuilding the search index) before any caller runs.

### Provider modules

A provider may be linked into the executable or loaded at run time. The module
exports exactly one symbol:

```c
wada_status wada_plugin_entry(uint32_t size, uint32_t abi,
                              wada_plugin *out, wada_err *err);
```

The host passes the size of its own `wada_plugin`, so an older module writes only
the fields it knows; the module reports the plugin ABI it speaks, the provider id
and the **store** ABI of the vtable it returns. The host validates all of it —
plugin ABI, struct size, id, store ABI, vtable header — before reading a single
function pointer. A mismatch is `WADA_ERR_ABI`, and the test suite loads a module
that lies about its ABI to prove the loader refuses it without touching the
vtable.

Decisions this forced:

- **Two version numbers, not one.** The plugin handshake ABI (`WADA_PLUGIN_ABI`)
  and the store vtable ABI (`WADA_STORE_ABI`) evolve independently: a module can
  speak a newer handshake and an older store, and the combination is either
  understood or refused with a precise error.
- **Load once, never unmap.** In C, a loaded module cannot be unloaded safely
  while any function pointer into it may still exist. The app loads at startup
  and closes at exit; changing providers means restarting. Anything else is a
  crash waiting for a slow frame. `--doctor` prints the provider source, id and
  both ABIs.
- **Relative paths resolve against the executable**, not the current directory:
  a shortcut or a file manager starts the app with an arbitrary working
  directory.
- **The module contains its own copy of the provider.** The executable links the
  provider statically by default; the module is built from the same sources
  (`wada_store_sqlite.c` plus the SQLite amalgamation), so the two cannot drift.
  The cost is a second copy of SQLite in the module (6.3 MB unstripped here).
  Dropping the static copy would make the app depend on a sibling file; the
  shipped app keeps the static build and treats `--provider` as the platform
  capability, not the default.
- **One exported symbol, no registration callback.** The module is data (a
  vtable) plus one function; it never links against the shell and gets no
  handles beyond the out-parameter. That is what makes the same file usable by
  the app, the tests and a future host.

The dynamic path is tested on Windows only so far; the loader also has a
`dlopen` branch, which the next Linux check will exercise.

### Record layout is a product decision

The first product stored whole notes as one record. The review measured why
that fails: 8 KB of body inline × 10,000 notes = 84 MB loaded before the first
frame, and a per-frame filter scan on top. The fixed layout splits metadata
from body:

```text
note/<id>       v1\n<updated>\n<title>     (the list loads only this)
note-body/<id>  the body, raw              (loaded when selected)
```

The store interface did not change to enable this — it already had `get` and
prefix iteration. The lesson: a good seam does not protect you from a bad
record layout; that decision belongs to the product, and it has to be revisited
once real volume is considered.

### Storage

`wada_iface.h` defines `wada_store_ops` — open/close, transactions, get/put/del,
prefix iteration, migrate, caps. It deliberately contains **no SQL**, no file
names, no row ids and no WAL details: those are SQLite's business. Search is a
capability flag, not a query language in the interface; a provider that cannot
rank returns `WADA_ERR_UNSUPPORTED` and the host falls back to its own index.

SQLite implements this as `providers/sqlite/` (amalgamation 3.54.1 vendored in
`vendor/sqlite/`), the only place that includes `sqlite3.h`. The core links it
in as a static vendor today; the same vtable can arrive from a dynamically
loaded module later without a core change.

What the provider proved: adding storage changed **no line in `src/core/` or
`src/shell/`**. The product and the provider were added beside them, and the
notes screen reads and writes through the interface. That is the first real
evidence the boundary is where it should be.

Products do not link the provider: boot publishes it as the `store` service,
and a product asks the registry for it. The provider header appears in exactly
two places — boot and the provider's own tests.

---

## 5. Scopes

Every registration is an effect owned by a scope:

```c
wada_scope *screen = wada_scope_create(wada_app_scope(app), "notes");
wada_sub *sub = wada_bus_subscribe(bus, "doc/saved", 0, on_saved, screen_state);
wada_scope_own(screen, wada_bus_cancel_owned, sub);
...
wada_scope_destroy(screen);  /* children first, then entries in reverse */
```

Why: unload, screen teardown and test isolation become the same mechanical
operation. There is no list of hand-written "remember to remove X" calls, and
no registration can outlive its owner by accident.

Rules:

- A registration inside a scope that is disposing returns 0 (invalid) instead
  of registering into a dead scope.
- Disposal is reverse-registration order and idempotent per token.
- Removing a service only marks it dead; the allocation stays owned by the
  registry, so a later scope token can never touch freed memory.

### Products are scopes

`wada_host` gives every product a child scope of the app. Switching products
is: leave the current product, destroy its scope, enter the next product with
a fresh scope. Commands, subscriptions and services a product registered go
with it. There is no product-specific teardown code in the host, and a product
cannot leak a command into the palette.

Two products now prove it: Notes and Learn share the core, the shell, the
store service and the settings file, and neither knows the other exists.


---

## 6. The bus

```c
wada_sub *wada_bus_subscribe(bus, "doc/saved", /*priority*/ 0, fn, user);
wada_bus_publish(bus, "doc/saved", &payload, sizeof payload);
```

- Delivery order: priority descending, then registration order.
- Publishing iterates a snapshot of the subscriber list. A listener may
  subscribe, cancel or publish during dispatch:
  - added during dispatch → not called by that dispatch;
  - cancelled during dispatch → skipped and freed only when the outermost
    dispatch ends;
  - nested publish → its own snapshot; no shared mutable iteration state.
- Payloads are borrowed for the duration of the call. Copy what you keep.

`app/frame` and `app/status` are the shell's topics; products use namespaced
topics (`notes/…`, `store/…`). Later, a second dispatch mode (waterfall/filter)
can be added for transforms; v1 has publish only, so there is exactly one
ordering rule to reason about.

---

## 7. Frame-loop rules

These come from the hazards catalogued across platforms and apply to every
product built on the core:

1. **No widget is destroyed inside its own handler.** Commands that close a
   panel enqueue `wada_app_defer`; the queue drains at the start of the next
   frame.
2. **No model is freed while a frame still reads it.** Detach first, free at a
   frame boundary.
3. **No lifecycle transition inside a draw callback.** Open/close/reload go
   through the deferred queue.
4. **Order is explicit.** Draw phases (background, content, overlays) and
   dispatch order are fixed by priority or phase, never by load order.
5. **The UI thread does no I/O.** Storage and network work is posted from a
   worker and applied at a frame boundary.
6. **A hidden window renders continuously.** Automation steps frames on a window
   nobody can click; without continuous rendering the frame call waits for an
   event that will never come once nothing needs a redraw. `--doctor` found this
   the first time its walk needed more than a couple of frames.

Tests drive the real frame loop through `wada_app_run_steps(app, compose, user,
frames, step, user)`: `step` runs after deferred work and before the draw, where
the drawing context is valid, so it can feed `cui_input_key`,
`cui_input_mouse_*` and `cui_input_text` and the same frame reacts. That is how
the host tests press a shortcut while the settings panel is open and assert it
does not reach the product.

---

## 8. The settings panel is a surface, not a product

The panel opens over whichever product is active, so it cannot be a product:
switching products disposes a product's scope, and a user who switched product
*from* the panel would delete the panel doing it. It is a shell **surface**:

```c
wada_app_set_surface(app, draw, panel, &panel->open);   /* shell only */
```

- The app draws it after the product and the status bar, before the palette.
- While `*open`, the app dispatches no shortcut and does not toggle the palette,
  so the dialog owns the keyboard. Esc and a click outside close it (CUI's
  modal behaviour).
- Opening and closing it touches no scope: its command lives in the panel's own
  scope and disappears with it.
- Changes are applied at the **next frame start** (`wada_app_defer`). Applying a
  theme or a language inside the frame that changed it would draw the widgets
  after the switch in the new style and the ones before it in the old — a
  single-frame seam that users see as a flicker.
- The panel and boot write the same keys (`ui/theme`, `ui/lang`,
  `app/product`). Boot still persists window size on exit. Two writers with one
  key set cannot disagree.

What it costs, stated plainly: one surface at a time (no dialog over the
settings dialog), no product-contributed sections (that needs a contribution
registry, a plugin feature). One visible quirk: CUI derives a modal's identity
from its title, so switching language re-creates the dialog and it animates in
once; the alternative (a fixed title) would mean an untranslated title.

What is tested, and what still is not: the widgets call four named actions
(`set_theme`, `set_language`, `set_product`, open/close) and the host test calls
the same ones after pressing real keys, so the queue → apply → persist path is
covered end to end, including the status bar and the title. A synthetic *click*
on the switch is still not part of the suite: a hidden window has no reliable
hit-testing story, and clicking by hard-coded coordinates would test the layout
constant, not the behaviour. The manual check is a screenshot.

---

## 9. Strings are data keyed by their English text

```c
static const wada_i18n_pair table[] = {
    {"Delete this note?", "Xóa ghi chú này?"},
};
wada_i18n_register(scope, table, count);   /* static data, owned by the scope */
wada_tr("Delete this note?");              /* lookup at draw time */
```

The key is the English text itself (the gettext model). Why:

- There is no second identifier to drift out of sync, and no lookup table to
  maintain next to every label.
- A missing translation degrades to readable English, not to `notes.new`.
- A product's table is static data registered with its scope, so translation
  strings follow the same disposal rule as commands and services.

The costs:

- Changing the English wording orphans its translation. The tests and `--doctor`
  make that visible instead of silent: `wada_tr` records every string that had no
  translation for the current language (bounded, deduplicated, and filtered so
  versions and ids do not count). `--doctor` then draws each product's declared
  screens — the first frame plus the commands a product lists in
  `wada_product.walk` (a review session, an add form, a delete confirmation) —
  and the settings panel, and prints `untranslated: N (screens drawn: M)`. The
  number is a lower bound: it counts what the walk draws, and a product that
  forgets to declare a screen is not covered.
- Two English strings with different meanings cannot share a translation
  (gettext needs a context for that; v1 has none).
- The table shape is `{en, vi}`. A third language changes the struct, which is
  fine while products are compiled in and would not be fine for product modules;
  that is a step-7 decision, not an accident to discover later.

Which strings are translated when:

- Command titles stay English at registration; the palette translates them at
  draw time, so a language switch needs no re-registration.
- Product titles are translated when the host enters the product (window title,
  status bar) and at draw time in the palette and the settings product list.
  The host registers every product's table for the life of the app, because a
  product's title must be readable while another product is active.
- Status messages are translated when they are set. The only runtime language
  switch is the settings panel, and it replaces the status with the language
  name — in the new language — so the bar cannot show stale text. The host test
  asserts exactly that. If another runtime switcher appears, it must do the
  same, or the status has to become a key-plus-arguments message; that is the
  honest boundary of the current design, not a hidden one.

**A table is not translation if the glyphs are missing.** The bundled Inter and
Geist fonts are Latin-only: rendering `Tiếng Việt` produced visible gaps
(`ă đ ơ ư` and every tone mark below the base letter). The fix is not a vendored
font and not a download: boot loads an OS font (Segoe UI, Arial, DejaVu, Noto —
whichever exists) and registers it as CUI's fallback for body, medium, display
and mono. Text keeps its typography; the fallback supplies the missing glyphs;
on a system without any of those files the app still runs with the original
coverage. The screenshot in `shots/settings-vi.png` is the evidence.

Not translated on purpose: provider diagnostics, the `--doctor` report, and the
`examples/` programs (they are the smallest contract, not a product).

---

## 10. Data safety

Three failures can destroy a user's notes, and all three were real in earlier
builds:

1. **Two processes.** Each one caches lists and product state; the second
   instance shows stale data, and both write settings. `wada_instance` holds
   `<data-dir>/wada.lock` open without sharing (Windows `CreateFile` with a zero
   share mode, POSIX `flock`) for the process lifetime. The second instance exits
   with code 2 and a log line naming the reason. `--doctor` is exempt on purpose:
   it is a bounded probe and must be able to run while the app is open. The lock
   is per data directory, not per executable, so an example cannot walk into the
   shipped app's database either.
2. **Ids that collide.** The old scheme was `<seconds>-<serial>`, with the
   serial reset on every open. A restart in the same second re-issued the
   previous run's first id, and creating a note overwrote its neighbour. Ids are
   now `<seconds>-<serial>-<seed>`, where the seed mixes time, pid, clock and
   address entropy once per process, and the record layer still checks the store
   and draws again on a hit — probability is not the guarantee. Both notes and
   cards caught the class of bug in tests.
3. **A record written back under the wrong key.** Cards and the todo example
   stored the full key (`card/…`) in `id` when created, but stripped the prefix
   when loading. Reviewing a card after a restart wrote it outside the prefix:
   the card looked reviewed until the next launch, then lost the review. The
   example had the same defect for toggles and clear-done. The rule is now
   explicit: **`id` is the suffix; the key is built by one function** — and a
   regression test reviews a reloaded card and reopens.

Plus one recovery feature: the provider backs the database up with
`VACUUM INTO` before a schema migration (kept, because a migration bug rewrites
every record) and once a day into `backups/<db>-daily.db` (replaced, so the
folder stays bounded). A failed pre-migration snapshot aborts the open — the one
moment where continuing without a copy would be the bug. `last_backup` reports
the path through the interface, so `--doctor` shows where the safety copy is
without the app knowing anything about SQLite files.

What is not claimed: the lock is tested in one process on Windows (`flock` is
written but has not run on a Linux machine), and `VACUUM INTO` cost grows with
the database — a 500 MB store would pause the first open of the day. The
migration branch is tested: the store test writes a v1 database with the raw
SQLite API (tests may know the vendor), opens it through the provider, and reads
the pre-v1 copy to confirm it is the old schema with the old record.

---

## 11. Rejected on purpose

- **Hot unload as a promise.** A C module cannot be unmapped while callbacks
  or threads may point into it. Load once; restart to change.
- **An OSGi-style dependency resolver.** Version-range wiring and refresh are a
  second product. Hard dependencies fail the plugin; soft ones only order it.
- **Ambient authority.** Obsidian and VS Code let a plugin own the process.
  Our plugins declare the services they require and receive only those
  handles.
- **Plugin-drawn UI.** A native shell cannot run plugin JavaScript. UI is data
  (contributions + declarative views) rendered by the host, as Raycast does.
- **An immortal ABI.** COM and VS Code pay for stability with permanent
  surface. We version and retire interfaces deliberately.
- **A plugin marketplace at v0.** Curation, signing and egress policy are a
  product decision, not a core feature.
- **Settings as a product.** A product scope dies on switch, so a settings
  product could not stay open while the user switches product — the one thing
  the panel must do.
- **One binary per language.** Compiling `wada-en.exe` and `wada-vi.exe` avoids
  the lookup entirely and throws away runtime switching, the untranslated
  counter and half the binary size.
- **A translation database (.po/.mo, ICU MessageFormat).** For two languages and
  in-tree products, a static table is smaller, has no parser, no plural-rule
  engine and no runtime file dependency. It becomes the wrong answer the day a
  product ships as a module.
- **Unloading a provider when `--provider` changes.** Restarting is one second;
  a use-after-unload is a crash on the user's machine. The loader unloads only
  at process exit.
- **A named mutex or port instead of a lock file.** A file lock is inspectable
  (`wada.lock` holds the pid), portable (share mode on Windows, `flock` on POSIX)
  and released by the OS on a crash. A named mutex works on Windows only; a
  port-based single-instance check needs a service to answer.
- **UUID4 for record ids.** A random id loses the creation-time prefix that
  makes listings readable, and it still needs the store check for the clock
  case. The seeded timestamp id gives ordering and restart safety without a
  second scheme.
- **A backup on every launch.** A copy per launch fills the disk for no extra
  safety; once a day plus once per migration covers the mistakes that actually
  happen.
- **Copying the `.db` file before opening.** With WAL enabled, the newest data
  lives in `-wal`, so a byte copy can be inconsistent. `VACUUM INTO` produces a
  consistent database by construction.
- **Making `--doctor` take the instance lock.** It would block diagnostics
  exactly when a user needs them (the app is open and something looks wrong).

---

## 12. Roadmap

| Step | Content | Proves | State |
|---|---|---|---|
| **1** | core + shell + demo | bus, scopes, registry, commands, palette, status, deferred work | done |
| **2** | SQLite provider + notes product | the interface is sufficient and does not leak SQL | done |
| **3** | learning product + product host + settings | the core is product-agnostic; switching is scope disposal | done |
| **4** | search (FTS5), split records, virtual list, log sink, schema/migrate | the interface evolves append-only and the data model scales | done |
| **5** | export/import, delete confirmation, host tests, doctor commands | the platform is safe, testable and portable | done |
| **6** | settings panel, i18n tables, provider modules | the platform is user-configurable and extensible without core changes | done |
| **7** | step hook + panel actions, declared doctor screens, status policy | automation presses real keys and walks every declared screen | done |
| **8** | instance lock, unique ids, key discipline, pre-migration and daily backups | data survives a second instance, a restart and a migration | done |
| **9** | records + ui kit (shared list/editor scaffold), one product shrunk and tested | duplication above the seam is removed without a framework | next |

Each step has one acceptance rule: no change outside the new product's folder
plus one registration line. If a step needs core changes, the boundary was
wrong — fix it then, while there is only one product.

Step 6 honoured it: the settings panel is shell code, the string tables live
with their products (`wada_product.strings`), and the module entry point is the
provider's own file. The core gained no knowledge of settings, languages or
DLLs; it gained a surface hook and a plugin handshake, both generic. Step 8
added a core id helper and a shell lock module: both generic, neither knows a
product — and the provider's backup is the provider's own policy, reached
through one appended query.

### Why multi-window is not in step 6

CUI owns one app per process, so a second window means a second process. Two
processes can open the same SQLite database (WAL supports it), but each product
caches its list and its `data_version` in memory: window B's new note would not
appear in window A until A reloads. The hard part is not creating a window; it
is **coherence** — a change channel between processes (WAL hooks, a change log
table, or polling a revision counter) and a rule for what each product does when
it sees a foreign change. Building the window first would prove the easy half
and hide the real problem, so it waits for a design step of its own.

A second reference is the `examples/` folder: `hello` (product lifecycle,
commands, shortcuts, bus) and `todo` (the store service and a versioned record
format). They are the teaching versions of the product contract and are built
by default as separate executables.

---

## 13. Sources

- VS Code: activation events, extension host — code.visualstudio.com/api
- Obsidian: obsidian-api repository (Plugin register* / onload/onunload)
- Raycast: manifest and extension components — developers.raycast.com
- Chrome: MDN WebExtensions manifest v3 (background, permissions, content scripts)
- JetBrains: plugin extension points, services, dynamic plugins docs
- Eclipse/OSGi: IExtensionRegistry, bundle lifecycle, service registry specs
- Emacs: Elisp hooks, advice, packaging/unloading manual
- Neovim: LSP/remote plugin docs, `api_level` and `:checkhealth` documentation
- SQLite: Virtual table modules (`iVersion`), loadable extensions
- Qt: QPluginLoader, Q_DECLARE_INTERFACE, plugin deployment rules
- COM: IUnknown/QueryInterface (Microsoft Learn)
- Vulkan: VkBaseInStructure/VkApplicationInfo; Linux `perf_event_attr.size`
- Extism memory model, Wasmtime C API, LSP specification
- Home Assistant integration manifest/config-entry docs
- WordPress plugin handbook (hooks, activation)
- Cordis: primer and tutorial (the plugin layer DSH is built on)
- Django: application registry, `AppConfig.ready`
- GNU gettext manual: msgid as key, fuzzy entries, untranslated reporting
- Qt: `tr()` / Linguist workflow (the same keying trade-off at a larger scale)
- Microsoft Learn: `LoadLibrary`, `GetProcAddress`, DLL search order, and the
  rule that a DLL is unloaded only when no pointer into it remains
- CUI: font roles and `cui_font_add_fallback` (vendored header); system font
  paths on Windows, macOS and Linux for graceful degradation
- SQLite: `VACUUM INTO` (consistent copy of a live database, including WAL),
  `PRAGMA user_version`, WAL and backup guidance
- Microsoft Learn: `CreateFileA` and `dwShareMode` (zero means exclusive), file
  locking caveats on Windows
- Linux `flock(2)`: advisory locks, `LOCK_NB`, release on close
