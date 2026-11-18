# Web UI tests

Scripted tests for `../ui/index.html` and `../ui/setup.html`. They load the
page in jsdom and talk to `mock.py`, a stand-in for the controller's HTTP
API. They do not need the board, and they are not a browser test: layout
and styling are only ever checked by eye on the real controller.

Needs Python 3 and [bun](https://bun.sh).

```sh
bun install
MOCK_CONN=1 MOCK_TZNAME=Europe/Warsaw python3 mock.py 8766 &
P=8766 bun test_dash.js      # prints PASS / FAIL lines
```

`./run_all.sh` runs every suite, each against a fresh mock started with the options it needs, and prints one
line per suite. `test_i18n.js` checks the translations (`docs/TRANSLATING.md`); give it `AP_P`, the port of a
second mock started with `MOCK_APONLY=1`, to include the setup page.

Most suites pass against the mock started as above. Some need it started
differently; the options are environment variables read at the top of
`mock.py`:

| Suite | Start the mock with |
|---|---|
| `test_fw.js`, `test_flow.js` | add `MOCK_HOLDOFF=1` |
| `test_time.js` | several mocks on ports 8766 to 8771, see `run_time_tests.sh` and the `MODE` values in the test |
| `test_setup.js`, `test_setup2.js` | `MOCK_APONLY=1` (serves the setup wizard at `/`) |
| `test_2g4.js` | `MOCK_RADIO=2g4` on port `P`; optionally a plain mock on `BLE_P` to also check the Bluetooth build is unchanged and the cross-build firmware warning |
| `test_clear.js` | add `MOCK_OTHER=1` (and once `MOCK_RADIO=2g4`) |
| `test_bthost.js` | `MOCK_RADIO=2g4` on port `P` |
| `test_autofill.js` | `MOCK_RADIO=2g4` on port `P` |
| `test_cfgreset.js` | `MOCK_CONN=1` alone |
| `test_osupdate.js` | `MOCK_CONN=1`; the mock keeps the OS-update state, `/__osu` patches it |
| `test_i18n.js` | `MOCK_CONN=1`; `AP_P` = port of a mock with `MOCK_APONLY=1` |
| `test_ble_empty.js` | `MOCK_NOCTL=1 MOCK_CONN=1` (a scan with no controller-like device) |

When the HTTP API changes, change `mock.py` and
`docs/FINAL_FIRMWARE_SPEC.md` together.
