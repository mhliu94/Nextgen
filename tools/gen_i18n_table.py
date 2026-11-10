#!/usr/bin/env python3
"""Regenerate src/core/i18n_table.inc from KTrader's trading_ui/i18n.py.

The legacy keys keep API error messages and labels identical to the Python server. Keys used only
by the rewritten front end live in src/core/i18n.cpp.

    KTRADER_REPO=/path/to/KTrader python3 tools/gen_i18n_table.py > src/core/i18n_table.inc
"""
import ast
import json
import os
import subprocess
import sys

repo = os.environ.get("KTRADER_REPO")
if not repo:
    sys.exit("Set KTRADER_REPO to a KTrader checkout.")
# Read the dictionary literal without importing the module (it depends on FastAPI).
source = open(os.path.join(repo, "trading_ui", "i18n.py"), encoding="utf-8").read()
I18N = None
for node in ast.parse(source).body:
    target = node.target if isinstance(node, ast.AnnAssign) else (node.targets[0] if isinstance(node, ast.Assign) else None)
    if isinstance(target, ast.Name) and target.id == "I18N":
        I18N = ast.literal_eval(node.value)
if not isinstance(I18N, dict):
    sys.exit("Could not find the I18N dictionary in trading_ui/i18n.py.")
try:
    commit = subprocess.check_output(["git", "-C", repo, "rev-parse", "--short", "HEAD"], text=True).strip()
except Exception:
    commit = "unknown"


def literal(text):
    # json.dumps yields a valid C++ string literal for these strings (UTF-8 kept as is).
    return json.dumps(text, ensure_ascii=False)


print(f"// Generated from KTrader trading_ui/i18n.py (commit {commit}) by tools/gen_i18n_table.py.")
print("// Do not edit by hand; new keys live in i18n.cpp.")
for lang in ("en", "zh"):
    print(f"static const KeyValue k_{lang}_legacy[] = {{")
    for key, value in I18N[lang].items():
        print(f"    {{{literal(key)}, {literal(value)}}},")
    print("};")
