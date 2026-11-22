# Learning from the builds

The products were not the goal; they were instruments. Build 2 (notes) and build
3 (learning) each forced a different part of the architecture to be real, and
each exposed a gap. This document reviews the builds as experiments, records
what they proved, and turns the friction into a ranked core backlog.

Rule for this document: every claim carries evidence from the code or a number.

---

## Method

For each build:

1. What did it exercise that nothing else had?
2. Where did the core, the interface or the shell get in the way?
3. Which "yes it works" claims are actually proven, and which are only asserted?
4. What would the next product need that this one did not?

---

## Build 1 — core + shell skeleton

**Exercised:** scopes, event bus, registry, commands, palette, status bar, a
compose callback, a hidden window loop.

**Proved:** the core compiles alone; a product screen can be built from
commands and CUI; the shell starts, draws and quits cleanly.

**Did not prove:** anything about data, about a second product, about failure.
It was a demo with no consumer. It is what made the next two builds necessary.

**Gap it hid:** the bus had subscribers only in tests. A bus nobody subscribes
to is untested infrastructure, not a feature.

---

## Build 2 — SQLite + notes

**Exercised:** the store interface, transactions, prefix iteration, ABI checks,
style of record encoding, CRUD from a real screen.

**Proved:**

- Adding storage changed **no line in `src/core/` or `src/shell/`**.
- The provider is the only file that includes `sqlite3.h`; the product includes
  neither the header nor SQL.
- A record format with a version line (`v1`) survives reopen; tests cover
  create/save/reopen/delete.
- The `{size, version}` check runs for real at open (`notes_open`).

**Friction / cons (with evidence):**

| Finding | Evidence | Cost |
|---|---|---|
| No search operation in the interface | `wada_iface.h` has `WADA_STORE_CAP_SEARCH` but no `search` slot | Notes search is a per-frame linear scan in the product |
| The whole collection is loaded at open | `notes.c: load_all` | `note` is ~8.4 KB (8 KB body); 10k notes ≈ 84 MB resident |
| Filter runs every frame | `notes_product.c: refresh_filter` called from `notes_frame` | At 10k notes: ~80 MB scanned per frame |
| Fixed-size records truncate silently | `notes_save` uses `snprintf` into `body[8192]` | A long paste loses text with no error |
| IDs can collide across instances | `note/%lld-%lld` with a per-process serial | Two app instances in the same second overwrite a key |
| Schema version is never used | no `migrate` caller in `src/apps/` or `src/` | "Migrations" exist but do nothing |
| The GUI build has no log sink | `WADA_LOG*` writes to `stderr`; window subsystem has no console | Every error is invisible in a real launch |
| Data path built in two places | `main.c` and each product build `dir/wada.db` | Adding a backup/portable mode means touching N files |

**What it validated about the architecture:** the store seam is right. The
interface is small enough to implement and sufficient for CRUD. The friction
was not at the seam; it was above it (product data model) and beside it
(search, paths, logging).

---

## Build 3 — learning + host + settings

**Exercised:** a second product, scope-per-product, product switching,
persisted settings, a scheduler with its own rules, a second connection to the
same database.

**Proved:**

- A second product costs ~530 lines (252 repo + 276 UI) and no core change.
- Product switching is scope disposal: leave, destroy scope, enter. No product
  names in the host.
- Settings persist window size, theme and the last product; relaunch reopens
  Learn (verified by `--doctor` + screenshot).
- Scheduling math is pure and tested (29 checks).

**Friction / cons:**

| Finding | Evidence | Cost |
|---|---|---|
| Products duplicate their scaffolding | `format_when` exists twice; path building twice; sidebar/editor patterns repeated | ~120–150 lines that should be a kit |
| The host is untested | no test creates a host; switching is only exercised manually | A regression in switching would ship |
| The bus still has no product consumer | `grep wada_bus` in `src/apps/` is empty | Cross-product reactions (e.g., notes search sees a new note) have no path |
| A failed product leaves an empty window | `wada_host.c: enter_index` sets a status and returns | No error screen, no fallback product |
| Browse/study rebuild state per frame | `learn_frame` recomputes counts and rows | Same scan problem as notes, less visible |
| Cards cannot be edited or deleted | no command | Product-level gap; also a test of the store's update path |
| Review queue is only built on entry | `start_study` | Cards that become due mid-session need a manual "Check again" |
| No delete confirmation or undo in either product | `cmd_delete` deletes immediately | One click loses a note |
| No unsaved-exit guard | `cmd_delete`/Quit | Quit while editing relies on autosave frames |

**What it validated:** product-agnosticism is real — the second product reused
the store, the shell, settings and the host without touching them. Scope
disposal is a good switching mechanism.

**What it revealed:** the repeated work has moved up a layer. The core is
stable; the *product kit* is missing. And infrastructure we built speculatively
(the bus) is still looking for its first consumer.

---

## Build 4 — settings panel, strings, provider modules

**Exercised:** settings as a shell surface, a second language switched at run
time, a provider loaded from a DLL, and the hidden-window harness extended to
the panel and to translation coverage.

**Proved:**

- The panel opens over both products and survives switching product from inside
  it, because it is not in a product scope. The hidden-window test asserts open,
  active and closed states, and that its command disappears with its scope.
- Two languages ship in one binary: `--lang vi` renders the notes list, the
  editor, learning and the panel in Vietnamese, and `--doctor --lang vi` reports
  0 untranslated strings after walking both products *and* the panel.
- Providers are replaceable without recompiling the app:
  `--provider wada_store_plugin.dll` passes the store probe, schema, counts and
  a full GUI session. A module that lies about its ABI is refused with
  `WADA_ERR_ABI` before its vtable is touched, and a missing file is an IO error
  with the path in it.
- The same vtable works linked and loaded: 26 module checks cover open, put,
  get, search, close and both failure modes.

**Did not prove:**

- That the panel's widgets work. The test opens, holds and closes it; it cannot
  flip the switch, change the select or press Done, because the shell has no
  scripted-input hook. Interaction is still a manual check with a screenshot as
  the evidence.
- A third language, a right-to-left script, or plural rules. Two languages with
  direct lookup prove the mechanism, not the general case.
- The POSIX `dlopen` branch. It compiles, but only the Windows `LoadLibrary` path
  ran here.

**The gap it hid:** the font. The first Vietnamese screenshot was full of holes:
the bundled Inter and Geist fonts contain neither `ă đ ơ ư` nor the tone marks,
so the table was complete and the screen was not. Nothing in the build or the
tests could see it; rendering it could. Fixed by loading an OS font as a CUI
fallback for the four text roles (no vendored font, no download, and a system
without it keeps the old coverage). The lesson generalises: **translation is not
done until the glyphs are drawn.**

**Measured this round:** `wada.exe` 2.79 → 2.81 MB stripped (+14 KB for the
panel, tables and module loader); the module is 6.32 MB unstripped because it
compiles SQLite a second time; checks 242 → 312; panel strings 24, notes table
36 pairs, learning table 34 pairs.

**What it cost, in code:** the shell gained a 4-line surface hook, the core
gained a handshake struct and no behaviour, and boot gained argument parsing for
two flags. The panel and the module loader are separate files. That is the
boundary holding: three features, no product-specific branch in the core.

---

## Build 5 — scripted input and walked screens

**Exercised:** `wada_app_run_steps` with real `cui_input_key` events, the
settings panel's four named actions, and a doctor walk that invokes
product-declared commands to reach screens one frame does not show.

**Proved:**

- A product shortcut does not fire while the panel is open: the test injects
  Ctrl+B into the frame, the product never runs, and after Escape the same
  shortcut runs once. That is the shell's blocking rule tested through the real
  input path, not asserted from code reading.
- Escape closes the dialog and returns the keyboard to the product.
- Theme, language and product from the panel go queue → next frame → persist;
  the test reads the values back from settings and the app state.
- A language switch leaves the status bar in the new language (the panel
  replaces it with the language name), so no stale text survives the switch.
- `--doctor` now draws 6 screens (both product roots, the delete confirmation,
  the review session, the add form, the panel) and reports
  `untranslated: 0 (screens drawn: 6)` in Vietnamese.

**Did not prove:** synthetic mouse clicks or Tab focus traversal (the widgets
are exercised through their actions, not their pixels); a third product
declaring its walk; the POSIX path.

**The bug it found:** a hidden window blocked forever. Once nothing needed a
redraw, `cui_app_frame` waited for events on a window nobody could click — the
doctor walk needed more than the four frames the old walk used, and hung. Fixed
by rendering hidden windows continuously, with a 12-frame regression check in the
host test. This is exactly the class of defect that "it opens and closes" tests
never reach.

**Measured this round:** checks 312 → 331 (host 31 → 50); no executable size
change worth reporting; doctor walk budget is computed from the declared
commands, not a constant.

---

## Build 6 — data safety

**Exercised:** a lock on the data directory, restart-safe ids, key discipline in
the two record layers, and provider-side backups.

**Proved:**

- A second instance exits 2 with a logged reason while the first one keeps
  running, and `--doctor` still runs against the open database (both checked
  with two real processes).
- A record created after close/open within the same second gets a different id:
  the notes test reproduces the old overwrite exactly and fails on the old
  scheme.
- A reloaded card is written back under its own key: review, close, reopen, and
  the interval survives. The old code lost that write outside the `card/`
  prefix, silently.
- The daily backup is a real database: the store test opens the file the
  provider reports and reads the record through the same interface.
- The examples no longer open the shipped app's database: boot takes a data
  subdirectory (`Wada Hello`, `Wada Todo`), verified by pointing
  `LOCALAPPDATA` at a scratch directory and checking three folders appear.

**Did not prove:** the POSIX `flock` branch, and the cost of `VACUUM INTO` on a
large store. The migration branch *is* tested: the store test writes a v1
database with the raw SQLite API, opens it through the provider, and reads the
pre-v1 copy to confirm it is the old schema with the old record.

**The bugs it found:** two, both silent data loss rather than visible errors.
The id scheme reset its serial per open, so a quick restart re-issued an id and
overwrote a note. Cards and the todo example wrote the full key on create but
stripped the prefix on load, so a review or a toggle after a restart landed on a
key nobody reads. Neither would ever surface as a crash or a test failure
without a reopen test — which is exactly why the step's tests are reopen tests.

**Measured this round:** checks 331 → 377; the manual two-instance check exits 2;
`--doctor` now reports `store: abi 3` and the backup path.

---

## Cross-build scorecard

| Claim | Evidence | Verdict |
|---|---|---|
| Core is product-agnostic | 2 products, 0 domain branches in the core after 5 builds | **proven** |
| Providers are replaceable | the same vtable works linked and loaded from a module; wrong ABI refused | **proven** |
| Scopes make teardown mechanical | product switch disposes commands/subscriptions; panel survives a switch | **proven by construction** |
| The bus defines reentrancy | tests only; no product subscribers | **speculative** |
| Store interface is sufficient | CRUD, search, schema and migration used by two products | **proven for these products** |
| Data model scales | metadata/body split, virtual list, cached filter | **proven at 10k-record math**, not measured at 10k |
| Errors are visible to users | status bar + `<data-dir>\log.txt`; provider failures logged | **proven** |
| The UI is configurable | theme, language and product persist and switch at run time | **proven** |
| The UI is testable | real key injection drives shortcuts, Escape and panel actions; no synthetic clicks | **proven for keys and actions** |
| Data survives a restart | ids unique across reopen; card review persists; daily backup is a readable database | **proven** |
| Data survives a second instance | two real processes; the second exits 2, doctor still runs | **proven manually, not in the suite** |

---

## Gaps, ranked by what they will cost

### Interface (the seam itself)

1. **No search operation.** A capability bit without an operation is a smell.
   Either add `search(txn, prefix, query)` with a documented degradation, or
   delete the bit and call search a product concern. Recommendation: add the
   operation; both current products need it and the provider (SQLite) can do it
   well.
2. **`migrate` has no owner.** The provider creates the schema at open, sets no
   version, and nobody calls migrate. Either make boot call it and record
   `PRAGMA user_version`, or remove it. Recommendation: make it real.
3. **No batch/transaction convenience.** Every put opens a transaction; loading
   is one transaction per product. Fine now; a records helper should own it.

### Core

4. **The bus has no consumer.** Keep it only if the next milestone uses it for
   change notifications (`record/changed`, `product/entered`). Otherwise it is
   dead weight. Recommendation: use it for change events — that also fixes the
   per-frame rescan.
5. **No log sink.** Boot should install a file sink under the data directory
   (`wada.log`, rotating), because the GUI build cannot write to a console.
6. **No schema/version ownership.** See interface #2.
7. **No single-instance guard.** Two instances can interleave settings writes
   and collide IDs. A named mutex/portable lock is cheap.

### Shell

8. **Command shortcuts are decorative.** `wada_app_command` stores a shortcut
   string; `wada_app.c` only handles Ctrl+K and Ctrl+Q. Ctrl+N/Ctrl+S do
   nothing. The palette looks like it has shortcuts; it does not.
9. **No confirmation primitive.** Destructive commands need a two-step confirm
   (or undo). Neither exists.
10. **No unsaved-exit guard.** Quit is immediate; autosave softens it but does
    not cover the first 20 frames or a crash.
11. **Host switching is untested.** A hidden-window harness can create the app,
    switch products, and assert command counts — CUI supports a hidden window.

### Product kit (the real duplication)

12. A `wada_ui` kit (list/detail scaffold, relative time, path building,
    status/error helpers) would remove ~150 lines per product.
13. Fixed-size records and load-all. Split records into metadata (list) and
    body (loaded on selection), or use variable-length storage. Use
    `cui_virtual_list` (currently unused) for rows.
14. Silent truncation. Either reject with an error or show a counter/limit.
15. ID collision across instances. Add a random suffix or use the store's
    monotonic timestamp.

### Ops

16. `--doctor` reports provider ABI and a probe, but not schema version,
    record counts, or log location. Add counts per prefix and the schema.
17. No backup. WAL protects integrity, not mistakes. `VACUUM INTO` on a
    schedule or on version upgrade is a cheap insurance.

---

## Fixed since this review

- **Shortcut dispatch** (finding 8): the shell parses each command's shortcut
  string (`Ctrl N`, `Ctrl+Shift+C`) and dispatches it each frame; the shipped
  products and both examples use it.
- **Boot glue extracted** (`src/shell/wada_boot.c`): data dir, settings,
  provider service, app, host and persistence live in one place, so an example
  app is two files instead of a fork of `main.c`.
- **Examples added** (`examples/hello`, `examples/todo`): the product contract
  now has two executable, minimal references; the todo example also proves the
  store service from a second, independent app.
- **Search is a store operation** (interface ABI 2): `search(prefix, query)`
  with FTS5 in the provider and `WADA_ERR_UNSUPPORTED` degradation. Notes no
  longer scan per keystroke or per frame.
- **Record layout split**: `note/<id>` holds metadata, `note-body/<id>` holds
  the body; the list loads ~200 bytes per note instead of ~8.4 KB, bodies load
  on selection, and legacy records migrate on open. Rows use
  `cui_virtual_list` and the filter only recomputes when the query or the data
  changes.
- **Log sink**: boot appends to `<data-dir>\log.txt`; the GUI build no longer
  loses every error.
- **Schema version and migration**: the provider records `PRAGMA user_version`,
  reports it through `schema_version()`, and migrates a v1 database (rebuilding
  the index) at open. `--doctor` shows it.
- **A bug the new tests caught**: on reload, the repos copied the full key
  (`note/…`, `card/…`, `todo/…`) into `id`, so the next save wrote
  `note/note/…`. Fixed in notes, cards and the todo example. This is the class
  of defect that only a reopen test finds.
- **Bodies are not capped at 8 KB any more**: the editor buffer grows on
  demand up to 4 MB, shows the character count, and never silently truncates on
  save. A 50,000-character body round-trips through store and backup in the
  tests.
- **Delete asks twice**: a second click (or "Keep", Escape, timeout, or another
  selection) settles it, so one click no longer destroys a note.
- **Export/Import**: a length-prefixed `.wnotes` file carries every title and
  body byte for byte; import creates new notes. The import test caught a second
  real bug — a length-delimited title running into the body bytes when
  sanitized — which is exactly why round-trip tests exist.
- **Host tests**: a hidden window (`wada_app_desc.hidden`) drives real product
  switching and asserts that the leaving product's commands disappear and the
  entering product's appear (21 checks).
- **Doctor lists commands**: boot enters each product hidden in a throwaway
  folder and reports ids with shortcuts, so `--doctor` now documents the real
  command surface without touching user data.
- **Settings UI**: `Ctrl+,` opens a shell surface with theme, language, product,
  data directory and database size. Theme and language apply at the next frame
  start (`wada_app_defer`) and persist to the same keys boot writes.
- **i18n**: English is the key; product tables are static data registered with
  the host for the life of the app; command titles are translated at draw time;
  `--doctor` reports how many drawn strings lack a translation, after walking
  the products *and* the panel.
- **Provider modules**: `wada_plugin_entry` + a versioned plugin struct, loaded
  with `LoadLibrary`/`dlopen`, validated field by field, refused with
  `WADA_ERR_ABI` on mismatch, never unmapped. `wada_store_plugin.dll` is built
  from the same sources as the linked provider.
- **System font fallback**: `wada_app` loads Segoe UI / Arial / DejaVu / Noto as
  a CUI fallback, because the bundled fonts have no Vietnamese coverage. The
  probe screenshot in `shots/settings-vi.png` is the check.
- **Module and i18n tests**: 26 module checks (good, wrong ABI, missing file),
  34 i18n checks (lookup, fallback, disposal, miss reporting, formatting), and
  the host test now covers the panel's open/block/close lifecycle.
- **A failed provider load is now recorded**: the GUI subsystem build has no
  usable stderr, so the reason goes to `<data-dir>\log.txt` as an error line and
  the process exits 1.

- **Scripted input**: `wada_app_run_steps` calls a step before each frame draws;
  the host test injects real keys and asserts shortcut suppression, Escape and
  post-close behaviour.
- **Hidden windows render continuously**: the frame loop no longer waits for
  events on a window nobody can click (found by the longer doctor walk).
- **Panel actions are named and tested**: the widgets call the same
  `set_theme`/`set_language`/`set_product` functions the test does, so queue →
  apply → persist is covered without synthetic clicks.
- **Doctor walks declared screens**: `wada_product.walk` names the commands that
  reach a review session, an add form or a confirmation; the report counts the
  screens drawn.
- **Status policy**: a language switch replaces the status with the language
  name, so no stale text remains; the test asserts it.

- **Data safety**:
  - `wada_instance` holds `<data-dir>/wada.lock` (share mode 0 / `flock`) for the
    process lifetime; the second start exits 2 and logs why. `--doctor` is
    exempt.
  - `wada_id_make` gives `<seconds>-<serial>-<seed>` ids, and notes/cards/todo
    draw again when the store already has the id.
  - Cards and todo build every key through one function, so a reloaded record is
    written back where it came from.
  - The provider uses `VACUUM INTO` for a pre-migration copy (kept) and a daily
    copy (replaced); ABI 3 reports the path, and `--doctor` prints it.
  - Examples take their own data subdirectories, so they cannot touch the
    shipped app's database; `WADA_NO_DAILY_BACKUP=1` keeps automation from
    writing snapshots.
  - Reopen tests reproduce the two data-loss bugs above; the store test opens
    the backup the provider reports.

Remaining after this pass: pre-migration and POSIX-lock tests, backup cost and
rotation for large stores, the records/ui kit, a third language, synthetic
clicks, and multi-window coherence. The P0/P1 lists above are otherwise
complete.

---

## The numbers behind the urgency

- `note` = 8.4 KB because `body[8192]` is inline. 1,000 notes = 8.4 MB;
  10,000 = 84 MB, loaded before the first frame.
- `refresh_filter` scans every title and body each frame. At 10k notes and
  60 fps that is ~5 GB/s of `strstr` work; at 1k it is tolerable (~500 MB/s),
  which is exactly why the demo felt fine.
- Cards are ~1.1 KB inline; 10k cards = 11 MB. The same design at a smaller
  constant.

The architecture is fine; the *record model* is the bottleneck. That is the
strongest lesson of the three builds: a good seam does not protect you from a
bad data layout.

---

## Ranked backlog

### P0 — done in earlier rounds (kept for the record)

1. **Search op in the store interface** — done (ABI 2, FTS5, degradation).
2. **Metadata/body split + virtual list + cached filter** — done.
3. **File log sink + visible errors** — done (provider failures now logged).
4. **Shortcut dispatch** — done; `--doctor` lists them.
5. **Schema version + migrate at boot** — done.

### P0 — done in round 5 (kept for the record)

6. **Scripted UI input** — `wada_app_run_steps`; real keys drive the frame loop.
7. **Status messages follow a language switch** — the panel replaces the status
   with the language name, asserted by the test.
8. **Walk declared screens in `--doctor`** — `wada_product.walk`, with the
   screen count printed next to the untranslated count.

### P0 — done in round 6 (kept for the record)

9. **Single-instance guard and collision-proof ids** — `wada_instance`, seeded
   ids checked against the store, and the key-prefix bug fixed in cards/todo.
10. **Backup on upgrade and daily** — `VACUUM INTO`, `backups/`, path in
    `--doctor` (ABI 3).

### P0 — do next (evidence is strong, scope is clear)

No open P0 items. The next work is the duplication list in P1.

### P1 — next cycle

11. `wada_records` helper (load/put/delete/iterate by prefix) to kill the repo
    duplication; then notes/cards shrink.
12. `wada_ui` kit: list/detail scaffold, relative time, path building, error
    banners; then two products shrink by ~150 lines each.
13. Unsaved-exit guard (autosave covers 20 frames, not the first ones).
14. Third language: change the pair shape to a language list
    (`{en, const char *texts[]}`) before any product ships strings from a module.
15. A v1 database fixture, so the pre-migration backup and migration path is
    tested instead of asserted.
16. POSIX checks: `wada_plugin_tests` and `wada_instance_tests` on a Linux box.
17. Settings: restore defaults, keyboard navigation, synthetic click helper if a
    hidden window can support it.
18. Large-store backup policy: measure `VACUUM INTO` on a 100–500 MB database,
    move it off the first frame if it costs too much.

### P2 — only with a consumer

19. Domain change events on the bus (`record/changed`) — or delete the bus. It
    still has no product subscriber after six builds.
20. Plugin signing, sandboxing, a marketplace.
21. Multi-window: needs a cross-process change channel first (see CORE §12).

---

## Decisions this review forces

1. **The panel is shell code, and stays that way.** A product-contributed
   settings section needs a contribution registry; that is the plugin feature we
   deliberately did not build.
2. **One language pair, not a translation engine.** `{en, vi}` is enough while
   products are compiled in. The moment a product ships as a module, the table
   shape becomes part of that module's ABI — decide it then, not now.
3. **The untranslated counter is a lower bound.** It counts what the doctor walk
   draws. Either walk every screen or label the number as partial.
4. **The bus has one more build to prove itself.** If the next feature does not
   need it, delete it: unused infrastructure is a lie about capability.
5. **Provider modules stay load-once, validated, and out of the shipped default.**
   The executable keeps the linked provider; the module proves the seam and
   serves as the deployment option.

---

## What not to build yet

- A marketplace, plugin signing or a sandbox: no third-party provider exists and
  egress policy is a product decision.
- Product modules (products loaded from DLLs). Provider modules prove the
  handshake; a product module would additionally need UI contribution rules,
  string-table ABI and scope wiring across the module boundary.
- Multi-window, sync, accounts, LLM features.
- More products. Two is still the right number: the next list is about data
  safety and duplication, not features.

---

## The meta-lesson

Six builds produced one pattern:

1. The first consumer proves the seam compiles.
2. The second consumer reveals what the seam is missing.
3. Duplication appears at the layer *above* the seam, not at the seam.
4. The features that look finished on screen (translation, settings) are the
   ones that hide untested edges: glyph coverage, stale status text, widgets no
   test can click.
5. Making the UI testable finds the bug the feature hid: the longer doctor walk
   hung on a hidden window that was waiting for events nobody could send.
6. Data bugs are silent. Nothing crashes; the record lands on a key nobody
   reads, or a re-issued id overwrites its neighbour. Only a reopen test sees
   them, and only if it writes after reopening.

That is why the next work is the shared kit — the duplication between notes and
cards — rather than another feature: the platform now survives restarts,
migrations and a second instance, and the remaining cost is the code every
product writes twice.
