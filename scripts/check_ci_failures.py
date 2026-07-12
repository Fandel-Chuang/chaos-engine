#!/usr/bin/env python3
"""
检查 GitHub Actions CI 失败的 Job，生成报告到 .ci-reports/ 目录。
用法: python3 scripts/check_ci_failures.py
"""
import json
import urllib.request
import os
import sys
from datetime import datetime

REPO = "Fandel-Chuang/chaos-engine"
API_BASE = f"https://api.github.com/repos/{REPO}/actions"
REPORT_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), ".ci-reports")


def api_get(url, timeout=15):
    """GET GitHub API，返回 JSON"""
    req = urllib.request.Request(url, headers={"Accept": "application/vnd.github+json"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read())


def fetch_runs(per_page=10):
    """获取最近的 CI 运行列表"""
    data = api_get(f"{API_BASE}/runs?per_page={per_page}")
    return data.get("workflow_runs", [])


def fetch_failed_jobs(run_id):
    """获取某次运行中失败的 Job 列表"""
    data = api_get(f"{API_BASE}/runs/{run_id}/jobs")
    failed = []
    for job in data.get("jobs", []):
        if job.get("conclusion") in ("failure", "cancelled", "timed_out"):
            failed.append(job)
    return failed


def fetch_job_logs(job_id, lines=30):
    """获取 Job 日志的最后 N 行"""
    try:
        url = f"{API_BASE}/jobs/{job_id}/logs"
        req = urllib.request.Request(url, headers={"Accept": "application/vnd.github+json"})
        with urllib.request.urlopen(req, timeout=10) as resp:
            content = resp.read().decode("utf-8", errors="replace")
            return content.strip().split("\n")[-lines:]
    except Exception as e:
        return [f"(日志获取失败: {e})"]


def main():
    os.makedirs(REPORT_DIR, exist_ok=True)

    print(f"正在查询 {REPO} 的 CI 运行...")
    runs = fetch_runs(10)
    if not runs:
        print("没有找到 CI 运行记录")
        return

    failed_runs = []
    for r in runs:
        conclusion = r.get("conclusion", "")
        if conclusion in ("failure", "cancelled", "timed_out"):
            failed_runs.append(r)

    timestamp = datetime.now().strftime("%Y-%m-%d_%H%M%S")
    report_path = os.path.join(REPORT_DIR, f"ci-failures-{timestamp}.md")
    summary_path = os.path.join(REPORT_DIR, "ci-failures-latest.md")

    lines = []
    lines.append("# ChaosEngine CI 失败报告")
    lines.append(f"- 仓库: {REPO}")
    lines.append(f"- 时间: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    lines.append(f"- 最近 {len(runs)} 次运行，失败 {len(failed_runs)} 次")
    lines.append("")

    if not failed_runs:
        lines.append("## ✅ 所有 CI 运行均通过，无需修复")
        # 清空历史失败任务列表
        todo_path = os.path.join(REPORT_DIR, "TODO-fix.md")
        with open(todo_path, "w") as f:
            f.write("# CI 失败待修复任务\n\n（当前无失败）\n")
    else:
        todo_items = []
        for r in failed_runs:
            run_id = r["id"]
            commit_msg = r.get("head_commit", {}).get("message", "?").split("\n")[0][:60]
            html_url = r.get("html_url", "?")
            branch = r.get("head_branch", "?")

            lines.append(f"## ❌ Run #{run_id}")
            lines.append(f"- 提交: `{commit_msg}`")
            lines.append(f"- 分支: {branch}")
            lines.append(f"- URL: {html_url}")
            lines.append("")

            failed_jobs = fetch_failed_jobs(run_id)
            for job in failed_jobs:
                job_name = job.get("name", "?")
                job_conclusion = job.get("conclusion", "?")
                job_url = job.get("html_url", "?")
                job_id = job.get("id", 0)

                lines.append(f"### ❌ Job: {job_name} ({job_conclusion})")
                lines.append(f"- URL: {job_url}")
                lines.append("")
                lines.append("```")
                log_lines = fetch_job_logs(job_id, 30)
                lines.extend(log_lines)
                lines.append("```")
                lines.append("")

                todo_items.append(f"- [ ] **{job_name}** (Run #{run_id}, 分支 {branch})\n  - 提交: {commit_msg}\n  - URL: {job_url}")

        # 写 TODO 文件
        todo_path = os.path.join(REPORT_DIR, "TODO-fix.md")
        with open(todo_path, "w") as f:
            f.write("# CI 失败待修复任务\n\n")
            f.write(f"更新时间: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n")
            f.write(f"待修复 Job 数: {len(todo_items)}\n\n")
            f.write("## 待修复列表\n\n")
            for item in todo_items:
                f.write(item + "\n\n")

    # 写报告文件
    content = "\n".join(lines)
    with open(report_path, "w") as f:
        f.write(content)
    with open(summary_path, "w") as f:
        f.write(content)

    print(f"\n报告已生成:")
    print(f"  详细报告: {report_path}")
    print(f"  最新汇总: {summary_path}")
    print(f"  待修复TODO: {todo_path}")
    print(f"\n统计: {len(runs)} 次运行，{len(failed_runs)} 次失败")


if __name__ == "__main__":
    main()
