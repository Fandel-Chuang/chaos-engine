#!/usr/bin/env python3
"""
给 C 头文件的函数声明批量补中文 Doxygen 注释 —— 用本地 qwen2.5-coder:7b。

安全设计（重要）:
1. 只在函数声明上方"插入"注释行, 绝不修改任何已有代码字符
2. 默认 dry-run, 必须显式 --write 才落盘
3. 落盘前后用 `gcc -fsyntax-only` 校验, 语法坏了自动回滚
4. 已有 /** */ 注释的声明直接跳过
5. 每个文件先备份到 <file>.bak

用法:
    python3 annotate_headers.py src_c/core/ce_math.h            # 预览
    python3 annotate_headers.py src_c/core/ce_math.h --write     # 落盘
    python3 annotate_headers.py src_c/core/ce_math.h --limit 5   # 只处理前5个
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import urllib.request

OLLAMA_URL = os.environ.get("OLLAMA_URL", "http://127.0.0.1:11434")
MODEL = os.environ.get("ANNOTATE_MODEL", "qwen2.5-coder:7b")

# 匹配函数声明: 返回类型 + ce_xxx( ... );   要求以分号结尾(声明而非定义)
DECL_RE = re.compile(
    r"^(?P<indent>[ \t]*)"
    r"(?P<decl>(?:CE_API\s+)?(?:const\s+)?[A-Za-z_][A-Za-z0-9_]*\s*\**\s*"
    r"(?P<name>ce_[a-z0-9_]+)\s*\([^;{]*\)\s*;)\s*$"
)

PROMPT = """为下面这个 C 函数声明写一句简洁的中文说明，用于 Doxygen 注释。

要求：
- 只输出一行中文说明文字，不超过 40 个字
- 不要输出 /* */ 或 /** */ 符号，不要输出函数名，不要输出任何英文标点
- 不要解释你的推理过程
- 直接描述这个函数做什么

函数声明：
{decl}
"""


def ask(decl: str) -> str:
    payload = {
        "model": MODEL,
        "prompt": PROMPT.format(decl=decl),
        "stream": False,
        "options": {"temperature": 0, "num_predict": 80, "num_ctx": 2048},
    }
    req = urllib.request.Request(
        f"{OLLAMA_URL}/api/generate",
        data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=180) as r:
        txt = json.load(r).get("response", "")

    # 清洗：去代码围栏、注释符、引号、换行
    txt = re.sub(r"```[a-z]*|```", "", txt)
    txt = re.sub(r"/\*+|\*+/|^\s*\*", "", txt, flags=re.M)
    txt = txt.strip().strip('"').strip("'")
    first = next((ln.strip() for ln in txt.splitlines() if ln.strip()), "")
    return first[:60]


def syntax_ok(path: str) -> tuple[bool, str]:
    """用 gcc 只做语法检查。头文件单独编译常因缺依赖报错，
    所以只把'新引入的错误'当失败 —— 通过前后对比实现。"""
    r = subprocess.run(
        ["gcc", "-fsyntax-only", "-I", "src_c", "-I", "src_c/public_api", path],
        capture_output=True, text=True,
    )
    return r.returncode == 0, r.stderr


def count_errors(stderr: str) -> int:
    return len(re.findall(r"error:", stderr))


def process(path: str, limit: int, write: bool) -> int:
    with open(path, encoding="utf-8") as f:
        lines = f.readlines()

    _, before_err = syntax_ok(path)
    base_errors = count_errors(before_err)

    out, added, idx = [], 0, 0
    while idx < len(lines):
        line = lines[idx]
        m = DECL_RE.match(line.rstrip("\n"))

        if not m:
            out.append(line)
            idx += 1
            continue

        # 上一行已是注释结尾 -> 跳过
        prev = next((l for l in reversed(out) if l.strip()), "")
        if prev.rstrip().endswith("*/") or prev.lstrip().startswith("//"):
            out.append(line)
            idx += 1
            continue

        if limit and added >= limit:
            out.append(line)
            idx += 1
            continue

        decl = m.group("decl")
        try:
            desc = ask(decl)
        except Exception as e:
            print(f"  [跳过] {m.group('name')}: {str(e)[:60]}", file=sys.stderr)
            out.append(line)
            idx += 1
            continue

        if not desc or not re.search(r"[\u4e00-\u9fff]", desc):
            print(f"  [跳过] {m.group('name')}: 模型未给出中文说明 ({desc[:30]!r})")
            out.append(line)
            idx += 1
            continue

        indent = m.group("indent")
        out.append(f"{indent}/** {desc} */\n")
        out.append(line)
        added += 1
        print(f"  ✓ {m.group('name'):<32} -> {desc}")
        idx += 1

    if not added:
        print("  (无需改动)")
        return 0

    if not write:
        print(f"\n  [dry-run] 将为 {added} 个声明加注释。加 --write 落盘。")
        return added

    shutil.copy2(path, path + ".bak")
    with open(path, "w", encoding="utf-8") as f:
        f.writelines(out)

    ok, after_err = syntax_ok(path)
    if count_errors(after_err) > base_errors:
        shutil.move(path + ".bak", path)
        print(f"\n  ✗ 语法检查新增错误，已回滚！\n{after_err[:400]}")
        return 0

    print(f"\n  ✓ 已写入 {added} 条注释（备份: {os.path.basename(path)}.bak，语法校验通过）")
    return added


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+")
    ap.add_argument("--write", action="store_true", help="真正落盘（默认只预览）")
    ap.add_argument("--limit", type=int, default=0, help="每个文件最多处理几个声明")
    a = ap.parse_args()

    total = 0
    for p in a.files:
        if not os.path.isfile(p):
            print(f"跳过不存在的文件: {p}", file=sys.stderr)
            continue
        print(f"\n===== {p} =====")
        total += process(p, a.limit, a.write)
    print(f"\n合计: {total} 条注释")


if __name__ == "__main__":
    main()
