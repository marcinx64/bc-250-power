#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The device list in devices/*.json (standard library only).

  python3 tools/devices.py check             validate every file against devices/schema.json
  python3 tools/devices.py table             print the compatibility table (Markdown)
  python3 tools/devices.py readme [PATH]     put the table between <!-- devices:start -->
                                             and <!-- devices:end --> in PATH (default README.md);
                                             does nothing if the markers are missing
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEVDIR = ROOT / "devices"
START, END = "<!-- devices:start -->", "<!-- devices:end -->"
LABEL = {
    "works": "Works",
    "works-untested-on-console": "Works on a spare board, console test pending",
    "expected": "Expected, untested",
    "help-wanted": "Help wanted",
    "does-not-work": "Does not work",
}
METHOD = {"ble": "Bluetooth LE", "2g4-esb1m": "2.4G dongle (ESB, 1 Mbit/s)",
          "bt-classic-page": "Bluetooth Classic paging", "other": "Other"}


def load_all():
    return [(p, json.loads(p.read_text())) for p in sorted(DEVDIR.glob("*.json")) if p.name != "schema.json"]


def _type_ok(v, t):
    types = t if isinstance(t, list) else [t]
    m = {"string": str, "array": list, "object": dict, "null": type(None)}
    return any(isinstance(v, m[x]) for x in types)


def validate(obj, schema, path="$"):
    """The subset of JSON Schema that schema.json uses."""
    errs = []
    if "enum" in schema and obj not in schema["enum"]:
        errs.append(f"{path}: {obj!r} not one of {schema['enum']}")
    if "type" in schema and not _type_ok(obj, schema["type"]):
        return errs + [f"{path}: expected {schema['type']}"]
    if isinstance(obj, str):
        if len(obj) < schema.get("minLength", 0):
            errs.append(f"{path}: empty")
        if "pattern" in schema and not re.search(schema["pattern"], obj):
            errs.append(f"{path}: {obj!r} does not match {schema['pattern']}")
    if isinstance(obj, dict):
        props = schema.get("properties", {})
        for r in schema.get("required", []):
            if r not in obj:
                errs.append(f"{path}: missing {r}")
        for k, v in obj.items():
            if k in props:
                errs += validate(v, props[k], f"{path}.{k}")
            elif schema.get("additionalProperties") is False:
                errs.append(f"{path}: unknown field {k}")
    if isinstance(obj, list):
        if len(obj) < schema.get("minItems", 0):
            errs.append(f"{path}: needs at least {schema['minItems']} items")
        for i, v in enumerate(obj):
            if "items" in schema:
                errs += validate(v, schema["items"], f"{path}[{i}]")
    return errs


def check():
    schema = json.loads((DEVDIR / "schema.json").read_text())
    bad, ids = 0, {}
    items = load_all()
    for p, d in items:
        errs = validate(d, schema)
        if isinstance(d, dict):
            if d.get("id") != p.stem:
                errs.append(f"id {d.get('id')!r} differs from the file name {p.stem!r}")
            if d.get("id") in ids:
                errs.append(f"duplicate id (also in {ids[d['id']]})")
            ids[d.get("id")] = p.name
        for e in errs:
            print(f"{p.name}: {e}")
        bad += bool(errs)
    print(f"{len(items)} device files, {bad} with errors")
    return 1 if bad else 0


def table():
    rows = ["| Device | Mode | Wake method | Status | Tested on | How to add |",
            "|---|---|---|---|---|---|"]
    for _, d in sorted(load_all(), key=lambda x: (x[1]["vendor"].lower(), x[1]["model"].lower())):
        for m in d["modes"]:
            rows.append("| {} {} | {} | {} | {} | {} | {} |".format(
                d["vendor"], d["model"], m.get("name", ""), METHOD.get(m["wake_method"], m["wake_method"]),
                LABEL[m["status"]], m.get("tested_on") or "-", m.get("how_to_add", "").replace("|", "/")))
    return "\n".join(rows)


def readme(path):
    p = Path(path)
    if not p.is_absolute() and not p.exists():
        p = ROOT / path
    text = p.read_text()
    if START not in text or END not in text:
        print(f"{p}: markers {START} / {END} not found; nothing changed")
        return 0
    a, b = text.index(START) + len(START), text.index(END)
    p.write_text(text[:a] + "\n\n" + table() + "\n\n" + text[b:])
    print(f"{p}: table updated")
    return 0


def main():
    a = sys.argv[1:]
    if a[:1] == ["check"]:
        sys.exit(check())
    if a[:1] == ["table"]:
        print(table())
    elif a[:1] == ["readme"]:
        sys.exit(readme(a[1] if len(a) > 1 else "README.md"))
    else:
        print(__doc__)
        sys.exit(1)


if __name__ == "__main__":
    main()
