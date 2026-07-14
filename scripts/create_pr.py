#!/usr/bin/env python3
"""GitHub PR 创建工具。

用法:
    GITHUB_TOKEN=*** python3 create_pr.py --title "标题" --head fix/branch --base master --body "描述"
    GITHUB_TOKEN=*** python3 create_pr.py --title "标题" --head fix/branch --body-file body.md

环境变量:
    GITHUB_TOKEN: GitHub Personal Access Token（必须）
"""

import argparse
import json
import os
import sys
import urllib.request
import urllib.error

REPO = "Fandel-Chuang/chaos-engine"


def main():
    parser = argparse.ArgumentParser(description="GitHub PR 创建工具")
    parser.add_argument("--title", required=True, help="PR 标题")
    parser.add_argument("--head", required=True, help="源分支名")
    parser.add_argument("--base", default="master", help="目标分支 (默认: master)")
    parser.add_argument("--body", default="", help="PR 描述 (直接传入)")
    parser.add_argument("--body-file", default=None, help="PR 描述 (从文件读取)")
    args = parser.parse_args()

    token = os.environ.get("GITHUB_TOKEN", "")
    if not token:
        print("错误: 请设置 GITHUB_TOKEN 环境变量", file=sys.stderr)
        sys.exit(1)

    body = args.body
    if args.body_file:
        with open(args.body_file, "r") as f:
            body = f.read()

    pr_data = {
        "title": args.title,
        "head": args.head,
        "base": args.base,
        "body": body,
    }

    url = f"https://api.github.com/repos/{REPO}/pulls"
    req = urllib.request.Request(
        url,
        data=json.dumps(pr_data).encode(),
        headers={
            "Accept": "application/vnd.github+json",
            "Authorization": f"Bearer {token}",
            "Content-Type": "application/json",
        },
    )

    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            result = json.loads(resp.read())
        print(f"PR #{result['number']} created: {result['html_url']}")
    except urllib.error.HTTPError as e:
        print(f"HTTP {e.code}: {e.read().decode()[:500]}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
