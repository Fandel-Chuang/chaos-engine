#!/usr/bin/env python3
"""
构建日志 warning 分类器 —— 规则聚合 + qwen3:1.7b 快速中文解读。

为什么用 1.7b: 实测 76 t/s(全程驻留显存), 分类这种短任务 0.2 秒出结果,
比 8b 快 5 倍且质量足够。真正的解析靠正则(确定性), LLM 只负责把
每一类 warning 翻译成人话 + 给修复建议。

用法:
    python3 classify_warnings.py /tmp/build4.log
    python3 classify_warnings.py /tmp/build4.log --no-llm     # 纯规则, 秒出
    python3 classify_warnings.py /tmp/build4.log --json       # 机器可读
"""
import argparse
import json
import os
import re
import sys
import urllib.request
from collections import defaultdict

OLLAMA_URL = os.environ.get("OLLAMA_URL", "http://127.0.0.1:11434")
MODEL = os.environ.get("CLASSIFY_MODEL", "qwen3:1.7b")

# gcc/clang warning 行: path:line:col: warning: 内容 [-Wflag]
WARN_RE = re.compile(
    r"^(?P<file>[^\s:][^:]*):(?P<line>\d+):(?:\d+:)?\s*warning:\s*(?P<msg>.*?)\s*"
    r"(?:\[(?P<flag>-W[a-z0-9-]+)\])?\s*$"
)

# 按危险程度分档 —— 依据是"是否可能真的导致运行期故障"
SEVERITY = {
    "-Wuse-after-free": "high",
    "-Wstringop-truncation": "high",
    "-Wstringop-overflow": "high",
    "-Warray-bounds": "high",
    "-Wformat-overflow": "high",
    "-Wuninitialized": "high",
    "-Wmaybe-uninitialized": "high",
    "-Wclobbered": "medium",
    "-Wunused-result": "medium",
    "-Wmissing-braces": "medium",
    "-Wsign-compare": "medium",
    "-Wvisibility": "low",
    "-Wunused-parameter": "low",
    "-Wunused-variable": "low",
    "-Wunused-but-set-variable": "low",
    "-Wcomment": "low",
}
SEV_ORDER = {"high": 0, "medium": 1, "low": 2, "unknown": 3}
SEV_ICON = {"high": "🔴", "medium": "🟡", "low": "🔵", "unknown": "⚪"}

PROMPT = """用中文解释这个 C 编译器警告，并给出修复方向。

警告类型：{flag}
示例信息：{sample}

要求：
- 第一行：这个警告说明什么问题（不超过 30 字）
- 第二行：怎么修（不超过 30 字）
- 只输出这两行，不要编号，不要其他内容
"""


def explain(flag: str, sample: str) -> tuple[str, str]:
    payload = {
        "model": MODEL,
        "prompt": PROMPT.format(flag=flag, sample=sample[:200]),
        "stream": False,
        "think": False,        # 关键: qwen3 不加这个会把答案写进被过滤的 thinking 段
        "options": {"temperature": 0, "num_predict": 120, "num_ctx": 2048},
    }
    req = urllib.request.Request(
        f"{OLLAMA_URL}/api/generate",
        data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=120) as r:
        txt = json.load(r).get("response", "")

    rows = [l.strip(" -*·") for l in txt.strip().splitlines() if l.strip()]
    # 1.7b 常自带 "警告说明：" / "修复方向：" 之类前缀，剥掉
    def clean(s: str) -> str:
        s = re.sub(r"^(警告说明|问题|说明|修复方向|修复|建议|方向)\s*[：:]\s*", "", s)
        return s.strip()

    what = clean(rows[0])[:60] if rows else ""
    how = clean(rows[1])[:60] if len(rows) > 1 else ""
    return what, how


def parse(path: str) -> dict:
    groups = defaultdict(list)
    with open(path, encoding="utf-8", errors="replace") as f:
        for raw in f:
            m = WARN_RE.match(raw.rstrip("\n"))
            if not m:
                continue
            flag = m.group("flag") or "(无标志)"
            groups[flag].append({
                "file": os.path.relpath(m.group("file")) if m.group("file").startswith("/") else m.group("file"),
                "line": int(m.group("line")),
                "msg": m.group("msg"),
            })
    return groups


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("logfile")
    ap.add_argument("--no-llm", action="store_true", help="跳过 LLM 解读，纯规则聚合")
    ap.add_argument("--json", action="store_true", help="输出 JSON")
    a = ap.parse_args()

    if not os.path.isfile(a.logfile):
        print(f"日志文件不存在: {a.logfile}", file=sys.stderr)
        sys.exit(1)

    groups = parse(a.logfile)
    if not groups:
        print("未发现 warning（或日志格式不匹配）")
        return

    items = []
    for flag, lst in groups.items():
        sev = SEVERITY.get(flag, "unknown")
        entry = {
            "flag": flag, "count": len(lst), "severity": sev,
            "files": sorted({w["file"] for w in lst}),
            "sample": lst[0]["msg"],
            "locations": [f"{w['file']}:{w['line']}" for w in lst[:5]],
        }
        if not a.no_llm:
            try:
                entry["what"], entry["how"] = explain(flag, lst[0]["msg"])
            except Exception as e:
                entry["llm_error"] = str(e)[:80]
        items.append(entry)

    items.sort(key=lambda x: (SEV_ORDER[x["severity"]], -x["count"]))

    if a.json:
        print(json.dumps({"total": sum(i["count"] for i in items), "groups": items},
                         ensure_ascii=False, indent=2))
        return

    total = sum(i["count"] for i in items)
    print(f"\n共 {total} 个 warning，{len(items)} 类（按危险程度排序）\n")
    for i in items:
        print(f"{SEV_ICON[i['severity']]} {i['flag']}  ×{i['count']}  [{i['severity']}]")
        if i.get("what"):
            print(f"    问题: {i['what']}")
        if i.get("how"):
            print(f"    修复: {i['how']}")
        if i.get("llm_error"):
            print(f"    (LLM 解读失败: {i['llm_error']})")
        print(f"    示例: {i['sample'][:90]}")
        print(f"    位置: {', '.join(i['locations'][:3])}"
              + (f" 等 {i['count']} 处" if i["count"] > 3 else ""))
        print()

    high = [i for i in items if i["severity"] == "high"]
    if high:
        print(f"⚠ 建议优先处理 {sum(i['count'] for i in high)} 个高危 warning: "
              + ", ".join(i["flag"] for i in high))


if __name__ == "__main__":
    main()
