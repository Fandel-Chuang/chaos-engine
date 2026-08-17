#!/usr/bin/env bash
# 拉取 ChaosEngine 的 PR / CI 状态。从 ~/.bashrc 提取 GITHUB_TOKEN 后调用 gh。
set -uo pipefail
REPO="Fandel-Chuang/chaos-engine"
GH="$HOME/.local/bin/gh"

TOKEN="${GITHUB_TOKEN:-}"
if [ -z "$TOKEN" ]; then
  TOKEN=$(grep -hoE 'GITHUB_TOKEN=["'"'"']?[A-Za-z0-9_]+' "$HOME/.bashrc" 2>/dev/null \
          | head -1 | sed -E 's/.*=["'"'"']?//')
fi
if [ -z "$TOKEN" ]; then
  echo "ERROR: 未找到 GITHUB_TOKEN（检查 ~/.bashrc）" >&2
  exit 1
fi
export GH_TOKEN="$TOKEN"

echo "===== 认证 ====="
"$GH" auth status 2>&1 | sed 's/^/  /'

echo
echo "===== 开放 PR ====="
"$GH" pr list --repo "$REPO" --state open \
  --json number,title,headRefName,isDraft,mergeable,statusCheckRollup \
  --template '{{range .}}#{{.number}} {{.title}}
   分支 {{.headRefName}} | draft={{.isDraft}} | mergeable={{.mergeable}}
{{end}}' 2>&1 | sed 's/^/  /'
echo "  (无输出 = 没有开放 PR)"

echo
echo "===== 最近 8 次 CI ====="
"$GH" run list --repo "$REPO" -L 8 \
  --json databaseId,conclusion,status,name,headBranch,createdAt \
  --template '{{range .}}{{.createdAt}} [{{.status}}/{{if .conclusion}}{{.conclusion}}{{else}}-{{end}}] {{.name}} @{{.headBranch}} (id {{.databaseId}})
{{end}}' 2>&1 | sed 's/^/  /'

echo
echo "===== master 最新一次 CI 详情 ====="
RID=$("$GH" run list --repo "$REPO" --branch master -L 1 --json databaseId -q '.[0].databaseId' 2>/dev/null)
if [ -n "${RID:-}" ]; then
  "$GH" run view "$RID" --repo "$REPO" 2>&1 | head -30 | sed 's/^/  /'
else
  echo "  未找到 master 的 CI run"
fi
