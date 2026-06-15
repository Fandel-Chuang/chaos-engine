#!/usr/bin/env python3
"""ci/rules/check_cpp_in_c.py — C++ 语法侵入检测

检查 src_c 下所有 .c/.h 文件是否包含 C++ 关键字。
C++ 关键字列表：class, namespace, template, new, delete, virtual, override
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC_C = os.path.join(ROOT, 'src_c')

# C++ 关键字列表（作为独立标识符检测，而非子串匹配）
CPP_KEYWORDS = [
    'class', 'namespace', 'template', 'new', 'delete',
    'virtual', 'override', 'noexcept', 'constexpr',
    'static_cast', 'dynamic_cast', 'reinterpret_cast', 'const_cast',
    'nullptr', 'decltype', 'typeid', 'typename',
    'explicit', 'mutable', 'friend', 'operator',
    'public:', 'private:', 'protected:',
]

# 排除项：这些是合法的 C 用法，不应误报
# - "new" 作为结构体成员名或变量名是合法的
# - "delete" 同理
# - 注释中的关键字不应检测
# - 字符串中的关键字不应检测
# - extern "C" 块中的内容也不应检测

# 编译正则：匹配独立单词边界的关键字
keyword_patterns = {}
for kw in CPP_KEYWORDS:
    # 匹配独立单词（前后非字母数字下划线）
    keyword_patterns[kw] = re.compile(r'\b' + re.escape(kw) + r'\b')

errors = 0

print("=" * 60)
print("  C++ Syntax Intrusion Detection")
print("  Scanning src_c/ for C++ keywords...")
print("=" * 60)
print()

# 收集所有 .c 和 .h 文件
files_to_check = []
for root, dirs, files in os.walk(SRC_C):
    for f in files:
        if f.endswith(('.c', '.h')):
            files_to_check.append(os.path.join(root, f))

files_to_check.sort()

for path in files_to_check:
    try:
        with open(path, encoding='utf-8', errors='ignore') as fh:
            lines = fh.readlines()
    except OSError as e:
        print(f"  WARNING: Cannot read {path}: {e}")
        continue

    in_multiline_comment = False
    in_extern_c_block = False

    for line_num, line in enumerate(lines, 1):
        stripped = line.strip()

        # 跳过空行
        if not stripped:
            continue

        # 处理多行注释
        if in_multiline_comment:
            if '*/' in stripped:
                in_multiline_comment = False
                # 继续处理 */ 之后的内容
                idx = stripped.index('*/') + 2
                stripped = stripped[idx:].strip()
                if not stripped:
                    continue
            else:
                continue

        # 检测多行注释开始
        if '/*' in stripped:
            # 如果 /* 在同一行有 */，则是单行注释块
            comment_start = stripped.index('/*')
            comment_end = stripped.find('*/', comment_start + 2)
            if comment_end != -1:
                # 移除注释部分
                stripped = stripped[:comment_start] + stripped[comment_end + 2:]
            else:
                # 多行注释开始
                in_multiline_comment = True
                stripped = stripped[:comment_start]

        # 移除单行注释
        if '//' in stripped:
            stripped = stripped[:stripped.index('//')]

        stripped = stripped.strip()
        if not stripped:
            continue

        # 检测 extern "C" 块（C 文件中合法的 C++ 语法）
        if 'extern' in stripped and '"C"' in stripped:
            in_extern_c_block = True
            continue
        if in_extern_c_block and '}' in stripped:
            in_extern_c_block = False
            continue
        if in_extern_c_block:
            continue

        # 检查 C++ 关键字
        for kw, pattern in keyword_patterns.items():
            if pattern.search(stripped):
                rel_path = os.path.relpath(path, ROOT)
                print(f"  ERROR: {rel_path}:{line_num}: C++ keyword '{kw}' found in C source")
                print(f"         {line.rstrip()}")
                errors += 1

print()
print("=" * 60)
if errors == 0:
    print("  PASS: No C++ keywords found in src_c/")
else:
    print(f"  {errors} C++ keyword(s) found in src_c/")
print("=" * 60)

sys.exit(0 if errors == 0 else 1)
