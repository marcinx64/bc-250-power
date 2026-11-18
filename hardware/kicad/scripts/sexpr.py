# SPDX-License-Identifier: GPL-3.0-or-later
"""Minimal S-expression helpers used by the KiCad generator scripts."""
import re


def tokenize(text):
    pos, n = 0, len(text)
    while pos < n:
        c = text[pos]
        if c in " \t\r\n":
            pos += 1
        elif c in "()":
            yield c
            pos += 1
        elif c == '"':
            j = pos + 1
            while text[j] != '"':
                if text[j] == "\\":
                    j += 1
                j += 1
            yield text[pos:j + 1]
            pos = j + 1
        else:
            m = re.compile(r'[^\s()"]+').match(text, pos)
            yield m.group(0)
            pos = m.end()


def parse(text):
    stack, cur = [], []
    for t in tokenize(text):
        if t == "(":
            stack.append(cur)
            cur = []
        elif t == ")":
            done = cur
            cur = stack.pop()
            cur.append(done)
        elif t.startswith('"'):
            cur.append(("str", t[1:-1]))
        else:
            cur.append(t)
    return cur


def val(x):
    return x[1] if isinstance(x, tuple) else x


def find_all(node, name):
    return [c for c in node if isinstance(c, list) and c and c[0] == name]


def find(node, name):
    r = find_all(node, name)
    return r[0] if r else None


def extract_symbol_text(lib_text, name):
    """Raw text of a top-level (symbol "name" ...) block of a .kicad_sym file."""
    key = '(symbol "%s"' % name
    i = lib_text.index(key)
    depth, j, instr = 0, i, False
    while True:
        c = lib_text[j]
        if instr:
            if c == "\\":
                j += 1
            elif c == '"':
                instr = False
        else:
            if c == '"':
                instr = True
            elif c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
                if depth == 0:
                    return lib_text[i:j + 1]
        j += 1
