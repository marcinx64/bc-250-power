# Translating the web UI

The web UI is written in English. Every other language is one JSON file in
`firmware/bc250_controller/ui/i18n/`. Adding a language needs no code change:
add the file, rebuild, flash. The UI picks the language from the browser, and
the user can change it in the Appearance card (and on the login and WiFi setup
screens). Anything you leave untranslated is shown in English.

## Add a language

```sh
python3 tools/i18n.py new de "Deutsch" "German"    # creates ui/i18n/de.json with every text empty
```

Then fill in the values (see "Rules" below), and check your work:

```sh
python3 tools/i18n.py check de      # errors and a list of what is still missing
python3 tools/i18n.py stats         # percent done per language
```

When the UI changes, bring a file up to date with `python3 tools/i18n.py update de`
(adds the new texts as empty entries; `--prune` also removes texts that no longer exist).

The code is the lowercase language code (`pl`, `de`, `pt-br`). For a language whose
plural rules `tools/i18n.py` does not know (`PLURALS` at its top), add them.

## The file

```json
{
  "@meta": { "code": "pl", "name": "Polski", "english_name": "Polish", "translators": ["Your Name"] },
  "Dashboard": "Pulpit",
  "Added {name}": "Dodano: {name}",
  "{n} device|{n} devices": { "one": "{n} urządzenie", "few": "{n} urządzenia", "many": "{n} urządzeń", "other": "{n} urządzenia" }
}
```

- The key is the exact English text, the value your translation. An empty value means "not translated yet".
- `@meta`: `name` is the language in its own language (shown in the language list), `translators` is a list of names.

### Rules

- **Placeholders** such as `{name}`, `{n}`, `{ip}` are replaced by the UI with a value. Keep them exactly,
  but you may move them to fit your grammar. `check` reports a missing or invented placeholder.
- **HTML**: a few texts contain `<b>…</b>` or `<a href="…">…</a>`. Keep the tags and the `href`, translate the words.
- **Plurals**: a key with `|` is `"singular|plural"` in English. The value is an object with one entry
  for every plural form of your language (CLDR names: `zero`, `one`, `two`, `few`, `many`, `other`).
  Polish needs `one`, `few`, `many`, `other`; English-like languages `one`, `other`; Japanese only `other`.
  `other` is also used for fractions. `{n}` is the number.
- **Keep names** as they are: BC-250, BLE, 2.4G, Bluetooth Classic, WiFi, NTP, PWR, RST, SENSE, PS_ON,
  and the network name `BC250-Setup` with its password.
- **Be consistent**: pick one word for each thing (wake list, hosts, "controller" = this board versus a
  game controller) and use it everywhere. Use sentence case, not Title Case, where your language does.
- **Tone**: short, plain, polite but not stiff, like the English. Buttons are verbs, messages are sentences.
- **Errors from the firmware** (texts such as `wrong password`, `list full`) are in the file too, because the UI
  shows them. A `{a}`, `{b}` in such a key is a number or name the firmware fills in.
- **Not translated**: the event log (Log page) stays English, and so do technical names.

## Try it without the hardware

The UI tests run the page in a simulator against a mock of the controller's API, which serves the same
catalogs from the files:

```sh
cd firmware/bc250_controller/ui_test
bun install
MOCK_CONN=1 python3 mock.py 8766 &
```

Open `http://127.0.0.1:8766/` in a browser (set a password at the first screen), choose your language in
Settings > Appearance, and walk through the pages. The setup page is at `/setup`.
`ui_test/run_all.sh` runs all UI tests, `bun test_i18n.js` the translation test.

## Submit

1. Run `python3 tools/i18n.py check <code>`; it must print no `ERROR`.
2. Add only your `ui/i18n/<code>.json` (and `PLURALS` in `tools/i18n.py` if needed) in a pull request.
   Mention the percentage from `stats`; partly translated files are welcome.
3. Native speakers reviewing a PR is the best quality check, so say who has looked at it.

## For developers: marking texts

Every text a user can see must go through the translation helpers, with the English text as a plain
double-quoted string literal (so `tools/i18n.py` finds it):

| Where | How |
|---|---|
| Static HTML text | add `data-i18n` to the element; the whole inner HTML is the text (only `<b>`, `<a>` inside; no entities, use the characters; no element that the script updates) |
| Static HTML attribute | `data-i18n-attr="aria-label title placeholder"` lists the attributes to translate |
| Text built by the script | `T("Added {name}", { name: n })` |
| Plural | `TN(n, "{n} device", "{n} devices")`; `{n}` is filled in; extra values as a 4th argument |
| A text kept in a table or chosen later | `N("text")` marks it for the catalog; show it with `T(table[x])` |
| Element built by the script | `tt(el, "text")`, `tl(el, "aria label")`, `tp(el, "placeholder")` set the text and remember it, so the language switch updates them |
| Text from the API | `error` fields are translated centrally; a new firmware error text in `err = "…"`, `sendErr(…)` or `fail(…)` is found by the tool |

One literal per call (no `"a" + "b"`), and never build a sentence from pieces: use one text with placeholders so
translators can reorder. Texts that depend on a number or a case get their own full sentences.
After a change run `python3 tools/i18n.py check` (new texts show as missing in the other languages) and
`bun test_i18n.js` in `firmware/bc250_controller/ui_test`. Texts the poll loop does not redraw must be
refreshed in `langApply()` in `ui/index.html`.

## How it works

`touch_buildinfo.py` (run by every build) reads `ui/i18n/*.json`, drops empty entries, gzips each file and
writes `src/i18n_data.cpp` (not committed). `src/web.cpp` serves `/i18n/index.json` and `/i18n/<code>.json`
without login (`docs/FINAL_FIRMWARE_SPEC.md`, last section). The page loads the list, picks the stored or
browser language, fetches that catalog and translates. English is the fallback and has no file.
