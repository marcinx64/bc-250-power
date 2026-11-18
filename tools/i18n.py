#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Translation catalogs of the web UI (docs/TRANSLATING.md). Python 3, standard library only.

  tools/i18n.py check [--strict] [CODE...]   validate catalogs; report missing and stale texts
  tools/i18n.py update CODE [--prune]        add the missing texts (empty), optionally drop stale ones
  tools/i18n.py new CODE "Native name" "English name"
  tools/i18n.py stats                        how much of each language is translated
  tools/i18n.py keys                         print the texts found in the sources (JSON)

The source texts are English. They are found in
  - firmware/bc250_controller/ui/index.html and setup.html: elements marked data-i18n, attributes listed in
    data-i18n-attr, and the calls T("..."), TN(n, "one", "other"), N("..."), tt(el, "..."), tl(el, "..."),
    tp(el, "..."), scanSay("...") in the scripts;
  - firmware/bc250_controller/src/*.cpp: the error texts the web API sends (err = "...", sendErr(..., "..."),
    fail(..., "..."), and the reset reasons).
"""
import glob
import html
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FW = os.path.join(ROOT, "firmware", "bc250_controller")
UI = os.path.join(FW, "ui")
SRC = os.path.join(FW, "src")
CAT = os.path.join(UI, "i18n")
PAGES = ("index.html", "setup.html")

# Plural categories (CLDR, cardinal) the UI needs per language. Unknown languages: only "other" is required.
PLURALS = {}
for _codes, _forms in (
    ("ja zh ko vi th id ms lo my km", ("other",)),
    ("en de nl sv da nb nn no fi et el hu tr bg sq eu af ka ta te ml mn ne sw ur", ("one", "other")),
    ("fr es it pt ca", ("one", "many", "other")),
    ("pl ru uk be cs sk lt", ("one", "few", "many", "other")),
    ("hr sr bs ro mo", ("one", "few", "other")),
    ("sl", ("one", "two", "few", "other")),
    ("lv", ("zero", "one", "other")),
    ("he", ("one", "two", "other")),
    ("ar", ("zero", "one", "two", "few", "many", "other")),
):
    for _c in _codes.split():
        PLURALS[_c] = _forms
ALL_FORMS = ("zero", "one", "two", "few", "many", "other")


def die(msg):
    sys.exit("i18n: " + msg)


# ---------------------------------------------------------------- extraction

STR = r'"(?:[^"\\\n]|\\.)*"'
VOID = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "source", "track", "wbr"}


def js_str(lit):
    return json.loads(lit)


def collapse(s):
    return " ".join(s.split())


def html_texts(text):
    """Keys from data-i18n elements and data-i18n-attr attributes of an HTML page; the scripts as a list."""
    keys, scripts = [], []
    tag_re = re.compile(r"<(!--.*?--|/?)([a-zA-Z][\w-]*)?((?:[^>\"']|\"[^\"]*\"|'[^']*')*)>", re.S)
    stack = []   # open data-i18n elements: [name, depth, start]
    pos = 0
    while True:
        m = tag_re.search(text, pos)
        if not m:
            break
        pos = m.end()
        if m.group(1).startswith("!--"):
            continue
        closing, name, attrs = m.group(1) == "/", (m.group(2) or "").lower(), m.group(3)
        if not name:
            continue
        if not closing and name in ("script", "style"):
            end = text.find("</" + name, pos)
            if end < 0:
                end = len(text)
            if name == "script":
                scripts.append(text[pos:end])
            pos = end
            continue
        if closing:
            if stack and stack[-1][0] == name:
                stack[-1][1] -= 1
                if stack[-1][1] == 0:
                    _, _, start = stack.pop()
                    keys.append(collapse(text[start:m.start()]))
            continue
        if re.search(r"(?<![\w-])data-i18n-attr=", attrs):
            for a in re.search(r'(?<![\w-])data-i18n-attr="([^"]*)"', attrs).group(1).split():
                v = re.search(r'(?<![\w-])%s="([^"]*)"' % re.escape(a), attrs)
                if v:
                    keys.append(html.unescape(v.group(1)))
        selfclose = attrs.rstrip().endswith("/")
        if stack and stack[-1][0] == name and not selfclose and name not in VOID:
            stack[-1][1] += 1
        if name in VOID or selfclose:
            continue
        if re.search(r"(?<![\w-])data-i18n(?![\w-])", attrs):
            if stack and stack[-1][0] == name:
                pass
            stack.append([name, 1, pos])
        elif stack and stack[-1][0] == name:
            pass   # counted above
    return keys, scripts


def script_texts(js):
    keys = []
    for m in re.finditer(r"(?<![\w$.])(?:T|N)\(\s*(%s)" % STR, js):
        keys.append(js_str(m.group(1)))
    for m in re.finditer(r"(?<![\w$.])TN\((?:[^,\"()]|\([^()]*\))+,\s*(%s)\s*,\s*(%s)" % (STR, STR), js):
        keys.append(js_str(m.group(1)) + "|" + js_str(m.group(2)))
    for m in re.finditer(r"(?<![\w$.])scanSay\(\s*(%s)" % STR, js):
        keys.append(js_str(m.group(1)))
    for m in re.finditer(r"(?<![\w$.])(?:tt|tl|tp)\((?:[^()\"]|\([^()]*\))*,\s*(%s)\s*\)" % STR, js):
        keys.append(js_str(m.group(1)))
    return keys


def c_string(lit):
    return json.loads(lit.replace("\\'", "'"))


def printf_to_template(fmt):
    n = [0]

    def rep(m):
        if m.group(0) == "%%":
            return "%"
        i = n[0]
        n[0] += 1
        return "{%s}" % chr(ord("a") + i)
    return re.sub(r"%%|%[-+ #0]*\d*(?:\.\d+)?(?:hh|h|ll|l|z|j|t)?[sdiuxXfcp]", rep, fmt)


def call_args(text, start):
    """Text of the call arguments, from just after the opening parenthesis to its closing one."""
    depth, i, instr = 1, start, False
    while i < len(text):
        c = text[i]
        if instr:
            if c == "\\":
                i += 1
            elif c == '"':
                instr = False
        elif c == '"':
            instr = True
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return text[start:i]
        i += 1
    return text[start:]


def statement(text, start):
    """The text from start up to the next semicolon outside a string."""
    i, instr = start, False
    while i < len(text):
        c = text[i]
        if instr:
            if c == "\\":
                i += 1
            elif c == '"':
                instr = False
        elif c == '"':
            instr = True
        elif c == ";":
            break
        i += 1
    return text[start:i]


def cpp_texts(sources):
    """User-visible texts of the web API errors in the C++ sources."""
    keys = []
    consts = {}
    for path, text in sources:
        for m in re.finditer(r"(?:const\s+char\s*\*\s*(?:const\s+)?|const\s+char\s+|#define\s+)([A-Z][A-Z0-9_]*)\s*(?:\[\s*\]\s*)?(?:=\s*)?(%s)\s*[;\n]" % STR, text):
            consts[m.group(1)] = c_string(m.group(2))

    def literals(s):
        return [c_string(x) for x in re.findall(STR, s)]

    for path, text in sources:
        text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
        text = re.sub(r"(?m)//.*$", "", text)
        # sendErr(code, "...") and fail(code, "...")
        for m in re.finditer(r"\b(?:sendErr|fail)\(", text):
            args = call_args(text, m.end())
            if re.match(r"\s*[A-Za-z_]\w*\s*,\s*[A-Za-z_]\w*\s*$", args):
                continue   # sendErr(code, variable)
            keys.extend(literals(args))
        # err = ...; error = ...;
        for m in re.finditer(r"\b(?:err|error)\s*=(?!=)\s*", text):
            rhs = statement(text, m.end()).strip()
            pf = re.match(r"(?:strPrintf|snprintf|std::string\s*\(\s*strPrintf)\s*\(\s*(?:\w+\s*,\s*\w+\s*,\s*)?(%s)" % STR, rhs)
            if pf:
                keys.append(printf_to_template(c_string(pf.group(1))))
                continue
            if re.fullmatch(r"[A-Z][A-Z0-9_]*", rhs):
                if rhs in consts:
                    keys.append(consts[rhs])
                continue
            if re.search(r"\?", rhs) or re.fullmatch(r"(?:std::string\s*\()?\s*%s\s*\)?" % STR, rhs):
                keys.extend(literals(rhs))
                continue
            # "text" + x, x + "text", std::string(x) + "text"
            parts, n = [], 0
            for item in re.split(r"\+(?=(?:[^\"]*\"[^\"]*\")*[^\"]*$)", rhs):
                item = item.strip()
                if re.fullmatch(STR, item):
                    parts.append(c_string(item))
                elif item:
                    parts.append("{%s}" % chr(ord("a") + n))
                    n += 1
            if any(re.fullmatch(STR, i.strip()) for i in re.split(r"\+(?=(?:[^\"]*\"[^\"]*\")*[^\"]*$)", rhs)):
                keys.append("".join(parts))
        # reset reasons: the UI shows them with a capital first letter
        for m in re.finditer(r"resetReasonName\s*\(\s*\)\s*\{", text):
            body = call_args(text, m.end() - 1 + 1) if False else text[m.end():m.end() + 1500]
            end = body.find("\n}")
            for lit in re.findall(r"return\s+(%s)\s*;" % STR, body[:end if end > 0 else len(body)]):
                s = c_string(lit)
                keys.append(s[:1].upper() + s[1:])
    return keys


def extract():
    """{key: sorted list of where it comes from}"""
    found = {}

    def add(k, where):
        if k and re.search(r"[A-Za-z]", k):
            found.setdefault(k, set()).add(where)
    for page in PAGES:
        with open(os.path.join(UI, page), encoding="utf-8") as f:
            text = f.read()
        keys, scripts = html_texts(text)
        for k in keys:
            add(k, page)
        for s in scripts:
            for k in script_texts(s):
                add(k, page)
    srcs = []
    for p in sorted(glob.glob(os.path.join(SRC, "*.cpp"))):
        with open(p, encoding="utf-8", errors="replace") as f:
            srcs.append((p, f.read()))
    for k in cpp_texts(srcs):
        add(k, "firmware")
    return {k: sorted(v) for k, v in found.items()}


# ---------------------------------------------------------------- catalogs

def sort_key(k):
    return (k.casefold(), k)


def forms_for(code):
    return PLURALS.get(code) or PLURALS.get(code.split("-")[0]) or ("other",)


def known_plural(code):
    return code in PLURALS or code.split("-")[0] in PLURALS


def is_plural(key):
    return "|" in key


def placeholders(s):
    return sorted(set(re.findall(r"\{(\w+)\}", s)))


def tags(s):
    return sorted(re.findall(r"<[^>]*>", s))


def load(code):
    path = os.path.join(CAT, code + ".json")
    if not os.path.exists(path):
        die("no catalog " + path)
    with open(path, encoding="utf-8") as f:
        try:
            return json.load(f, object_pairs_hook=lambda p: dict(p)), path
        except ValueError as e:
            die("%s: invalid JSON: %s" % (path, e))


def codes():
    return sorted(os.path.splitext(os.path.basename(p))[0] for p in glob.glob(os.path.join(CAT, "*.json")))


def done(key, value):
    if isinstance(value, dict):
        return bool(value) and all(isinstance(v, str) and v.strip() for v in value.values())
    return isinstance(value, str) and bool(value.strip())


def blank(key, code):
    return {f: "" for f in forms_for(code)} if is_plural(key) else ""


def write(path, meta, entries):
    out = {"@meta": meta}
    for k in sorted(entries, key=sort_key):
        out[k] = entries[k]
    with open(path, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, indent=2)
        f.write("\n")


def check_one(code, src, strict):
    """(errors, warnings, missing, stale) of one catalog."""
    cat, path = load(code)
    errs, warns = [], []
    meta = cat.get("@meta")
    if not isinstance(meta, dict):
        errs.append('"@meta" object missing')
        meta = {}
    else:
        if meta.get("code") != code:
            errs.append('@meta code "%s" does not match the file name "%s"' % (meta.get("code"), code))
        for f in ("name", "english_name"):
            if not isinstance(meta.get(f), str) or not meta.get(f).strip():
                errs.append("@meta %s missing" % f)
        if not isinstance(meta.get("translators"), list):
            errs.append("@meta translators must be a list")
        elif not meta["translators"]:
            warns.append("@meta translators is empty")
    forms = forms_for(code)
    if not known_plural(code):
        warns.append('no plural rules known for "%s" in tools/i18n.py: only "other" is required' % code)
    for key, val in cat.items():
        if key == "@meta":
            continue
        if key not in src:
            continue
        if is_plural(key):
            one, other = key.split("|", 1)
            allowed = set(placeholders(one)) | set(placeholders(other))
            if isinstance(val, str):
                if val.strip():
                    errs.append("plural text needs an object with %s: %r" % ("/".join(forms), key))
                continue
            if not isinstance(val, dict):
                errs.append("plural text must be an object: %r" % key)
                continue
            bad = [f for f in val if f not in ALL_FORMS]
            if bad:
                errs.append("unknown plural form %s in %r" % (bad, key))
            filled = [f for f in val if isinstance(val[f], str) and val[f].strip()]
            if filled:
                miss = [f for f in forms if f not in filled]
                if miss:
                    errs.append("plural forms missing (%s) in %r" % (", ".join(miss), key))
                for f in filled:
                    extra = set(placeholders(val[f])) - allowed
                    if extra:
                        errs.append("%s form of %r has unknown placeholder %s" % (f, key, ", ".join("{%s}" % x for x in sorted(extra))))
        else:
            if isinstance(val, dict):
                errs.append("text is not plural, value must be a string: %r" % key)
                continue
            if not isinstance(val, str):
                errs.append("value must be a string: %r" % key)
                continue
            if not val.strip():
                continue
            if placeholders(val) != placeholders(key):
                errs.append("placeholders differ: %r -> %r" % (key, val))
            if "<" in key and tags(val) != tags(key):
                errs.append("HTML tags differ: %r -> %r" % (key, val))
            if val != val.strip() and key == key.strip():
                warns.append("leading or trailing space: %r" % val)
    missing = sorted((k for k in src if k not in cat or not done(k, cat[k])), key=sort_key)
    stale = sorted((k for k in cat if k != "@meta" and k not in src), key=sort_key)
    if stale:
        warns.append("%d stale text(s) not used any more (tools/i18n.py update %s --prune)" % (len(stale), code))
    if strict and missing:
        errs.append("%d text(s) not translated" % len(missing))
    return errs, warns, missing, stale


def cmd_check(args):
    strict = "--strict" in args
    sel = [a for a in args if not a.startswith("--")] or codes()
    src = extract()
    bad = False
    for code in sel:
        errs, warns, missing, stale = check_one(code, src, strict)
        total = len(src)
        print("%s: %d of %d translated, %d missing, %d stale" % (code, total - len(missing), total, len(missing), len(stale)))
        for w in warns:
            print("  warning: " + w)
        for k in missing[:20] if not strict else missing:
            print("  missing: " + repr(k))
        if not strict and len(missing) > 20:
            print("  ... and %d more (use --strict to list all)" % (len(missing) - 20))
        for k in stale:
            print("  stale: " + repr(k))
        for e in errs:
            print("  ERROR: " + e)
        bad = bad or bool(errs)
    if not sel:
        print("no catalogs in " + CAT)
    return 1 if bad else 0


def cmd_update(args):
    prune = "--prune" in args
    rest = [a for a in args if not a.startswith("--")]
    if len(rest) != 1:
        die("usage: update CODE [--prune]")
    code = rest[0]
    cat, path = load(code)
    src = extract()
    meta = cat.pop("@meta", None)
    if not isinstance(meta, dict):
        die("%s has no @meta" % path)
    added = [k for k in src if k not in cat]
    stale = [k for k in cat if k not in src]
    for k in added:
        cat[k] = blank(k, code)
    for k in list(cat):   # a plural entry from a catalog of another language: add the forms this language needs
        if is_plural(k) and isinstance(cat[k], dict):
            for f in forms_for(code):
                cat[k].setdefault(f, "")
    if prune:
        for k in stale:
            del cat[k]
    write(path, meta, cat)
    print("%s: %d added, %d stale %s" % (code, len(added), len(stale), "removed" if prune else "kept (use --prune to remove)"))
    return 0


def cmd_new(args):
    if len(args) != 3:
        die('usage: new CODE "Native name" "English name"')
    code, name, ename = args
    if not re.fullmatch(r"[a-z]{2,3}(-[a-z0-9]{2,8})*", code):
        die("the code must be lowercase, like pl or pt-br")
    path = os.path.join(CAT, code + ".json")
    if os.path.exists(path):
        die(path + " exists")
    os.makedirs(CAT, exist_ok=True)
    src = extract()
    write(path, {"code": code, "name": name, "english_name": ename, "translators": []}, {k: blank(k, code) for k in src})
    print("created %s with %d texts to translate" % (path, len(src)))
    if not known_plural(code):
        print('note: no plural rules known for "%s"; add them to PLURALS in tools/i18n.py' % code)
    return 0


def cmd_stats(args):
    src = extract()
    for code in codes():
        cat, _ = load(code)
        n = sum(1 for k in src if k in cat and done(k, cat[k]))
        meta = cat.get("@meta") or {}
        print("%-8s %-20s %3d %%  (%d of %d)" % (code, meta.get("english_name", ""), round(100 * n / len(src)) if src else 100, n, len(src)))
    return 0


def cmd_keys(args):
    print(json.dumps(extract(), ensure_ascii=False, indent=1, sort_keys=True))
    return 0


def main(argv):
    cmds = {"check": cmd_check, "update": cmd_update, "new": cmd_new, "stats": cmd_stats, "keys": cmd_keys}
    if len(argv) < 2 or argv[1] not in cmds:
        print(__doc__)
        return 2
    return cmds[argv[1]](argv[2:])


if __name__ == "__main__":
    sys.exit(main(sys.argv))
