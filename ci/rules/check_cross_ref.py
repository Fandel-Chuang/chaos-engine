#!/usr/bin/env python3
"""ci/rules/check_cross_ref.py — 跨目录引用检测

检查规则：
1. src_c 中是否有文件引用了 src_cpp（纯 C 内核不应依赖 C++ 编辑器）
2. src_cpp 是否直接引用了 src_c 内部头文件（只能引用 public_api）
"""

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC_C = os.path.join(ROOT, 'src_c')
SRC_CPP = os.path.join(ROOT, 'src_cpp')

errors = 0

# ---- 规则 1: src_c 中是否有文件 include 了 src_cpp ----
print("=" * 60)
print("  Rule 1: src_c must NOT include src_cpp")
print("=" * 60)

for root, dirs, files in os.walk(SRC_C):
    for f in files:
        if f.endswith(('.c', '.h')):
            path = os.path.join(root, f)
            try:
                with open(path, encoding='utf-8', errors='ignore') as fh:
                    for i, line in enumerate(fh, 1):
                        if 'src_cpp' in line and '#include' in line:
                            print(f"  ERROR: {path}:{i}: src_c includes src_cpp")
                            errors += 1
            except OSError as e:
                print(f"  WARNING: Cannot read {path}: {e}")

if errors == 0:
    print("  PASS: No src_c -> src_cpp references found.")
print()

# ---- 规则 2: src_cpp 是否直接引用了 src_c 内部头文件 ----
print("=" * 60)
print("  Rule 2: src_cpp must only include public_api headers")
print("=" * 60)

cpp_errors = 0
for root, dirs, files in os.walk(SRC_CPP):
    for f in files:
        if f.endswith(('.cpp', '.hpp', '.h')):
            path = os.path.join(root, f)
            try:
                with open(path, encoding='utf-8', errors='ignore') as fh:
                    for i, line in enumerate(fh, 1):
                        if '#include' in line and 'src_c/' in line and 'public_api' not in line:
                            print(f"  ERROR: {path}:{i}: editor includes internal header")
                            cpp_errors += 1
            except OSError as e:
                print(f"  WARNING: Cannot read {path}: {e}")

if cpp_errors == 0:
    print("  PASS: No src_cpp -> src_c internal header references found.")
else:
    errors += cpp_errors

print()
print("=" * 60)
if errors == 0:
    print("  ALL CHECKS PASSED")
else:
    print(f"  {errors} ERROR(S) FOUND")
print("=" * 60)

sys.exit(0 if errors == 0 else 1)
