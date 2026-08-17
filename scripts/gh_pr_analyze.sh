#!/usr/bin/env bash
# 分析指定 PR：基本信息、CI 结论、与 master 的差异、冲突判定（只读，不改仓库）
set -uo pipefail
REPO="Fandel-Chuang/chaos-engine"
GH="$HOME/.local/bin/gh"

TOKEN="${GITHUB_TOKEN:-}"
if [ -z "$TOKEN" ]; then
  TOKEN=$(grep -hoE 'GITHUB_TOKEN=["'"'"']?[A-Za-z0-9_]+' "$HOME/.bashrc" 2>/dev/null \
          | head -1 | sed -E 's/.*=["'"'"']?//')
fi
[ -z "$TOKEN" ] && { echo "ERROR: 未找到 GITHUB_TOKEN" >&2; exit 1; }
export GH_TOKEN="$TOKEN"

for PR in "$@"; do
  echo "════════════════════ PR #$PR ════════════════════"
  "$GH" pr view "$PR" --repo "$REPO" \
    --json number,title,author,createdAt,updatedAt,headRefName,baseRefName,mergeable,mergeStateStatus,additions,deletions,changedFiles,body \
    --template '标题: {{.title}}
作者: {{.author.login}}
创建: {{.createdAt}}   更新: {{.updatedAt}}
分支: {{.headRefName}} -> {{.baseRefName}}
可合并: {{.mergeable}} / 状态 {{.mergeStateStatus}}
改动: +{{.additions}} -{{.deletions}} 跨 {{.changedFiles}} 个文件

--- 描述 ---
{{.body}}
' 2>&1

  echo "--- 改动文件 ---"
  "$GH" pr diff "$PR" --repo "$REPO" --name-only 2>&1 | head -40

  echo "--- CI 检查 ---"
  "$GH" pr checks "$PR" --repo "$REPO" 2>&1 | head -15

  echo
done
