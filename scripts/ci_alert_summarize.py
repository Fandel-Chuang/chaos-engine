#!/usr/bin/env python3
"""
CI 告警 LLM 结构化摘要模块 —— 用本地 ollama 把冗长邮件正文压成结构化 JSON。

设计原则:
1. 纯本地推理, 不花钱、不出网、无 API key
2. LLM 失败绝不能拖垮告警链路 —— 任何异常都退回规则解析 (fallback)
3. 规则解析先跑, LLM 只做增强 (提炼失败原因), 保证最坏情况也有可用输出

被 monitor_ci_email.py 调用; 也可单独测试:
    python3 ci_alert_summarize.py <告警md文件>
"""
import json
import os
import re
import sys
import urllib.error
import urllib.request

OLLAMA_URL = os.environ.get("OLLAMA_URL", "http://127.0.0.1:11434")
# 8b 做结构化抽取实测零错误; 可用 CI_SUMMARY_MODEL 覆盖
MODEL = os.environ.get("CI_SUMMARY_MODEL", "qwen3:8b")
TIMEOUT = int(os.environ.get("CI_SUMMARY_TIMEOUT", "120"))

# ---------------------------------------------------------------- 规则解析


def strip_noise(body: str) -> str:
    """砍掉 GitHub 通知邮件的固定尾巴和 token —— 这些占了正文一半以上"""
    body = re.sub(r"--\s*You are receiving this because.*$", "", body, flags=re.S)
    body = re.sub(r"Manage your GitHub Actions notifications.*$", "", body, flags=re.S)
    body = re.sub(r"email_token=[A-Z0-9]+", "", body)
    body = re.sub(r"https?://\S*email_source=\S*", "", body)
    return re.sub(r"\s+", " ", body).strip()


def rule_parse(subject: str, body: str) -> dict:
    """不依赖 LLM 的确定性解析 —— 这是 fallback 的底座"""
    clean = strip_noise(body)
    out = {
        "repo": None, "workflow": None, "duration": None,
        "run_url": None, "commit": None, "branch_or_pr": None,
        "failed_jobs": [], "passed_jobs": [], "summary": None,
    }

    m = re.search(r"Repository:\s*([\w.-]+/[\w.-]+)", clean)
    if m:
        out["repo"] = m.group(1)
    m = re.search(r"Workflow:\s*(.+?)\s+(?:Duration|Finished|Jobs):", clean)
    if m:
        out["workflow"] = m.group(1).strip()
    m = re.search(r"Duration:\s*(.+?)\s+Finished:", clean)
    if m:
        out["duration"] = m.group(1).strip()
    m = re.search(r"(https://github\.com/[\w.-]+/[\w.-]+/actions/runs/\d+)", clean)
    if m:
        out["run_url"] = m.group(1)
    m = re.search(r"\(([0-9a-f]{7,40})\)", subject)
    if m:
        out["commit"] = m.group(1)

    # 主题形如 "PR run failed: <workflow> - <描述> (sha)"
    m = re.search(r"(PR run failed|run failed|failed):\s*(.+?)\s*\([0-9a-f]{7,}\)", subject)
    if m:
        out["branch_or_pr"] = m.group(2).strip()

    # Jobs 列表: "* <名字> failed (N annotations)"
    for name, status in re.findall(r"\*\s*(.+?)\s+(failed|succeeded)\b", clean):
        (out["failed_jobs"] if status == "failed" else out["passed_jobs"]).append(name.strip())

    return out


# ---------------------------------------------------------------- LLM 增强

PROMPT = """你是 CI 告警分析助手。下面是一封 GitHub Actions 失败通知邮件的正文。
请只输出一个 JSON 对象，不要输出任何解释文字、不要用 markdown 代码块。

JSON 字段要求：
- "cause": 字符串，用一句中文说明最可能的失败原因（依据失败的 job 名推断，例如"Gateway 集成测试未通过"）
- "severity": 字符串，只能是 "high"/"medium"/"low" 之一。编译或测试失败=high，仅告警/lint=medium，其他=low
- "action": 字符串，用一句中文给出建议的下一步操作

邮件正文：
{body}
"""


def llm_enhance(clean_body: str) -> dict:
    """调本地 ollama 提炼失败原因。任何失败都抛异常, 由调用方兜底。"""
    payload = {
        "model": MODEL,
        "prompt": PROMPT.format(body=clean_body[:1500]),
        "stream": False,
        "think": False,          # 关键: 不加这个, qwen3 会把答案写进被过滤的 thinking 段
        "options": {"temperature": 0, "num_predict": 300, "num_ctx": 8192},
    }
    req = urllib.request.Request(
        f"{OLLAMA_URL}/api/generate",
        data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
        raw = json.load(r).get("response", "")

    # 模型偶尔仍会包 ```json 围栏, 容错剥掉
    txt = raw.strip()
    txt = re.sub(r"^```(?:json)?\s*|\s*```$", "", txt).strip()
    m = re.search(r"\{.*\}", txt, re.S)
    if not m:
        raise ValueError(f"LLM 未返回 JSON: {txt[:120]}")
    data = json.loads(m.group(0))

    sev = str(data.get("severity", "")).lower()
    return {
        "cause": str(data.get("cause", ""))[:200] or None,
        "severity": sev if sev in ("high", "medium", "low") else None,
        "action": str(data.get("action", ""))[:200] or None,
    }


# ---------------------------------------------------------------- 对外接口


def summarize(subject: str, body: str) -> dict:
    """返回结构化告警 dict。llm_ok 字段标明 LLM 是否成功参与。"""
    info = rule_parse(subject, body)
    info["subject"] = subject
    info["llm_ok"] = False

    try:
        info.update({k: v for k, v in llm_enhance(strip_noise(body)).items() if v})
        info["llm_ok"] = True
    except Exception as e:
        # LLM 挂了不影响告警送达, 仅标注原因
        info["llm_error"] = str(e)[:120]
        if info["failed_jobs"]:
            info["cause"] = "、".join(info["failed_jobs"]) + " 未通过"
            info["severity"] = "high"
        info.setdefault("action", "查看 run_url 中的 job 日志")

    return info


SEV_ICON = {"high": "🔴", "medium": "🟡", "low": "🔵"}


def render(info: dict) -> str:
    """渲染成适合 QQ 推送的紧凑文本"""
    icon = SEV_ICON.get(info.get("severity") or "", "🔴")
    lines = [f"{icon} CI 失败 | {info.get('repo') or '未知仓库'}"]

    if info.get("branch_or_pr"):
        lines.append(f"变更: {info['branch_or_pr']}")
    if info.get("commit"):
        lines.append(f"提交: {info['commit']}")
    if info.get("failed_jobs"):
        lines.append(f"失败 job: {', '.join(info['failed_jobs'])}")
    if info.get("passed_jobs"):
        lines.append(f"通过: {len(info['passed_jobs'])} 个")
    if info.get("cause"):
        lines.append(f"原因: {info['cause']}")
    if info.get("action"):
        lines.append(f"建议: {info['action']}")
    if info.get("duration"):
        lines.append(f"耗时: {info['duration']}")
    if info.get("run_url"):
        lines.append(f"详情: {info['run_url']}")
    if not info.get("llm_ok"):
        lines.append("(规则解析, LLM 未参与)")

    return "\n".join(lines)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    with open(sys.argv[1], encoding="utf-8") as f:
        content = f.read()

    m = re.search(r"^- 主题:\s*(.+)$", content, re.M)
    subj = m.group(1) if m else ""
    # 正文 = 去掉 md 头部的元信息行
    body = re.sub(r"^#.*$|^- (主题|时间):.*$", "", content, flags=re.M).strip()

    result = summarize(subj, body)
    print("===== 结构化 JSON =====")
    print(json.dumps(result, ensure_ascii=False, indent=2))
    print("\n===== 推送文本 =====")
    print(render(result))
