#!/usr/bin/env python3
"""GitHub Actions CI 查询工具。

用法:
    GITHUB_TOKEN=xxx python3 ci_query.py              # 查最新 PR/分支的 CI 状态
    GITHUB_TOKEN=xxx python3 ci_query.py --pr 10       # 查指定 PR 的 CI 状态
    GITHUB_TOKEN=xxx python3 ci_query.py --branch master  # 查指定分支
    GITHUB_TOKEN=xxx python3 ci_query.py --watch       # 持续轮询直到完成
    GITHUB_TOKEN=xxx python3 ci_query.py --merge 10    # 等 CI 通过后自动合入 PR

环境变量:
    GITHUB_TOKEN: GitHub Personal Access Token（必须）
"""

import argparse
import json
import os
import sys
import time
import urllib.request


REPO = "Fandel-Chuang/chaos-engine"


def _headers(token: str) -> dict:
    return {
        "Accept": "application/vnd.github+json",
        "Authorization": f"Bearer {token}",
    }


def _api_get(url: str, token: str) -> dict:
    req = urllib.request.Request(url, headers=_headers(token))
    with urllib.request.urlopen(req, timeout=15) as resp:
        return json.loads(resp.read())


def _api_put(url: str, token: str, data: dict) -> dict:
    req = urllib.request.Request(
        url,
        data=json.dumps(data).encode(),
        method="PUT",
        headers={**_headers(token), "Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=15) as resp:
        return json.loads(resp.read())


def get_runs(token: str, branch: str | None = None, per_page: int = 5) -> list[dict]:
    """获取最近的 CI runs。"""
    url = f"https://api.github.com/repos/{REPO}/actions/runs?per_page={per_page}"
    if branch:
        url += f"&branch={branch}"
    data = _api_get(url, token)
    return data.get("workflow_runs", [])


def get_jobs(token: str, run_id: int) -> list[dict]:
    """获取指定 run 的所有 job。"""
    url = f"https://api.github.com/repos/{REPO}/actions/runs/{run_id}/jobs"
    data = _api_get(url, token)
    return data.get("jobs", [])


def get_pr(token: str, pr_num: int) -> dict:
    """获取 PR 信息。"""
    url = f"https://api.github.com/repos/{REPO}/pulls/{pr_num}"
    return _api_get(url, token)


def format_run(run: dict, jobs: list[dict] | None = None) -> str:
    """格式化输出 run 信息。"""
    status = run.get("status", "?")
    conclusion = run.get("conclusion", "?")
    branch = run.get("head_branch", "?")
    created = run.get("created_at", "?")
    html_url = run.get("html_url", "")

    emoji = {"success": "✅", "failure": "❌", "cancelled": "⚠️"}.get(conclusion, "🔄")

    lines = [f"{emoji} [{branch}] {status}/{conclusion}  ({created})"]
    lines.append(f"   {html_url}")

    if jobs:
        for job in jobs:
            jc = job.get("conclusion") or job.get("status", "?")
            jn = job.get("name", "?")
            j_emoji = {"success": "✅", "failure": "❌", "cancelled": "⚠️", "skipped": "⏭️"}.get(jc, "🔄")
            lines.append(f"   {j_emoji} {jn}")

    return "\n".join(lines)


def check_ci(token: str, branch: str | None = None, pr_num: int | None = None) -> dict:
    """查 CI 状态，返回最新 run 信息。"""
    if pr_num:
        pr = get_pr(token, pr_num)
        branch = pr.get("head", {}).get("ref", "")

    runs = get_runs(token, branch=branch)
    if not runs:
        print(f"未找到 CI run" + (f" (branch={branch})" if branch else ""))
        return {}

    run = runs[0]
    run_id = run["id"]
    jobs = get_jobs(token, run_id)
    print(format_run(run, jobs))

    return {"run": run, "jobs": jobs}


def watch_ci(token: str, branch: str | None = None, pr_num: int | None = None, interval: int = 15, timeout: int = 600) -> bool:
    """持续轮询 CI 直到完成。"""
    if pr_num:
        pr = get_pr(token, pr_num)
        branch = pr.get("head", {}).get("ref", "")

    start = time.time()
    attempt = 0
    while time.time() - start < timeout:
        attempt += 1
        runs = get_runs(token, branch=branch)
        if not runs:
            print(f"[{attempt}] 等待 CI 触发...")
            time.sleep(interval)
            continue

        run = runs[0]
        status = run.get("status", "?")
        run_id = run["id"]

        if status == "completed":
            jobs = get_jobs(token, run_id)
            conclusion = run.get("conclusion", "?")
            print(format_run(run, jobs))

            all_ok = conclusion == "success"
            for job in jobs:
                jc = job.get("conclusion", "")
                if jc in ("failure", "cancelled"):
                    all_ok = False
            return all_ok
        else:
            print(f"[{attempt}] {status}...")
            time.sleep(interval)

    print("⏰ 超时")
    return False


def merge_pr(token: str, pr_num: int) -> bool:
    """合入 PR（squash merge）。"""
    url = f"https://api.github.com/repos/{REPO}/pulls/{pr_num}/merge"
    try:
        result = _api_put(url, token, {
            "commit_title": f"Merge PR #{pr_num}",
            "merge_method": "squash",
        })
        print(f"✅ PR #{pr_num} 已合入: {result.get('message', '?')}")
        print(f"   SHA: {result.get('sha', '?')}")
        return True
    except Exception as e:
        print(f"❌ 合入失败: {e}")
        return False


def main():
    parser = argparse.ArgumentParser(description="GitHub Actions CI 查询工具")
    parser.add_argument("--pr", type=int, help="指定 PR 编号")
    parser.add_argument("--branch", type=str, help="指定分支名")
    parser.add_argument("--watch", action="store_true", help="持续轮询直到 CI 完成")
    parser.add_argument("--merge", type=int, help="等 CI 通过后自动合入指定 PR")
    parser.add_argument("--interval", type=int, default=15, help="轮询间隔（秒）")
    parser.add_argument("--timeout", type=int, default=600, help="轮询超时（秒）")
    args = parser.parse_args()

    token = os.environ.get("GITHUB_TOKEN", "")
    if not token:
        print("错误: 请设置 GITHUB_TOKEN 环境变量", file=sys.stderr)
        sys.exit(1)

    if args.merge:
        pr_num = args.merge
        print(f"监控 PR #{pr_num} 的 CI 状态，通过后自动合入...")
        ok = watch_ci(token, pr_num=pr_num, interval=args.interval, timeout=args.timeout)
        if ok:
            merge_pr(token, pr_num)
        else:
            print("CI 未通过，不合入")
            sys.exit(1)
    elif args.watch:
        ok = watch_ci(token, branch=args.branch, pr_num=args.pr, interval=args.interval, timeout=args.timeout)
        sys.exit(0 if ok else 1)
    else:
        check_ci(token, branch=args.branch, pr_num=args.pr)


if __name__ == "__main__":
    main()
