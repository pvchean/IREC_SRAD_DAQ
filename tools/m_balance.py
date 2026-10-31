#!/usr/bin/env python3
"""A rough check of a MATLAB file when MATLAB is not installed: each block keyword has an end, and each
name that is used in a callback exists as a nested function. It is NOT a parser: it finds the simple mistakes.

  m_balance.py FILE.m
"""
import re
import sys

s = open(sys.argv[1]).read()
lines = []
for l in s.split("\n"):
    out, q, i = "", None, 0
    while i < len(l):
        c = l[i]
        if q:
            if c == q:
                if i + 1 < len(l) and l[i + 1] == q:
                    i += 1
                else:
                    q = None
        else:
            if c == "%":
                break
            if c == '"' or (c == "'" and (i == 0 or not (l[i - 1].isalnum() or l[i - 1] in ")]}'._"))):
                q = c
            else:
                out += c
        i += 1
    lines.append(out)

stack, bad = [], []
for n, l in enumerate(lines, 1):
    t = re.sub(r"\([^()]*\)", "()", l)
    t = re.sub(r"\([^()]*\)", "()", t)
    for m in re.finditer(r"\b(function|if|for|while|switch|try|end)\b", t):
        w = m.group(1)
        pre = t[:m.start()]
        if w == "end":
            if pre.count("(") > pre.count(")") or pre.count("[") > pre.count("]") or pre.count("{") > pre.count("}"):
                continue
            if not stack:
                bad.append(f"line {n}: extra end")
            else:
                stack.pop()
        else:
            stack.append((w, n))
funcs = set(re.findall(r"^\s*function\s+(?:\[?[\w,\s~]*\]?\s*=\s*)?(\w+)", s, re.M))
cb = set(re.findall(r"@(\w+)", s)) - {"(", ""}
missing = sorted(c for c in cb if c not in funcs)
print(f"blocks still open at the end: {stack[-5:] or 'none'}; problems: {bad[:5] or 'none'}")
print(f"functions: {len(funcs)}; callbacks (@name) with no function of that name: {missing or 'none'}")
sys.exit(1 if stack or bad or missing else 0)
