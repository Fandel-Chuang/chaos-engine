"""CI 触发器 - spec 6.2.2（Gitee 免费版适配）。

Gitee 免费版 Go 不支持手动触发流水线的 API（POST /repos/{repo}/builds 返回 405），
也不支持 commit status API（GET /repos/{repo}/commits/{sha}/status 返回 404）。

适配方案：通过 git push 空 commit 触发 Gitee Go CI，再轮询 commit 的构建状态。
核心原则：测试流程必须通过 Git CI 完成，LoopEngine 只触发/轮询/解析，不本地执行测试脚本。
复用 .gitee-ci.yml 现有 5 job（build-and-test/release-build/lua-lint/memcheck），不新增不修改。
"""

from __future__ import annotations

import asyncio
import logging
import re
import subprocess
import time

import httpx

logger = logging.getLogger(__name__)


class CITrigger:
    """Gitee Go CI 触发器（Gitee 免费版适配）。

    通过 git push 空 commit 触发流水线，轮询 Gitee API 查构建状态。
    git push 使用已有 SSH key 认证（~/.ssh/id_ed25519_gitee），无需额外凭证。
    """

    API_BASE = "https://gitee.com/api/v5"

    # CI 终态状态值
    TERMINAL_STATUSES = {"completed", "failure", "success", "cancelled", "skipped"}

    # 运行中状态值
    RUNNING_STATUSES = {"running", "pending", "queued", "in_progress", "waiting"}

    def __init__(
        self,
        gitee_token: str,
        repo: str = "zhong-fangdao/chaos-engine",
        chaos_engine_dir: str = "/home/zhongfangdao/chaos-engine",
        trigger_branch: str = "master",
    ) -> None:
        """初始化 CI 触发器。

        Args:
            gitee_token: Gitee API 访问令牌。
            repo: 仓库全名（owner/repo）。
            chaos_engine_dir: ChaosEngine 项目根目录（git 仓库路径），用于执行 git 命令。
            trigger_branch: 默认触发分支。
        """
        self.gitee_token = gitee_token
        self.repo = repo
        self.chaos_engine_dir = chaos_engine_dir
        self.trigger_branch = trigger_branch
        self.api_base = self.API_BASE

    def _headers(self) -> dict[str, str]:
        """构建请求头。"""
        return {
            "Authorization": f"token {self.gitee_token}",
            "Content-Type": "application/json",
            "Accept": "application/json",
        }

    def _params(self) -> dict[str, str]:
        """构建查询参数（Gitee API v5 要求 access_token 查询参数认证）。"""
        return {"access_token": self.gitee_token}

    async def trigger_pipeline(self, branch: str | None = None) -> dict:
        """通过 git push 空 commit 触发 Gitee Go CI。

        Gitee 免费版不支持 POST /repos/{repo}/builds API（返回 405），
        改为在 chaos_engine_dir 下执行 git commit --allow-empty + git push，
        利用 Gitee Go 的 push 触发器自动启动 CI。

        Args:
            branch: 触发分支名，默认使用 self.trigger_branch。

        Returns:
            dict: 触发结果，格式为:
                {triggered: True, branch: str, commit_sha: str, method: "git_push"}

        Raises:
            RuntimeError: git commit 或 git push 失败。
        """
        branch = branch or self.trigger_branch

        # 1. git commit --allow-empty
        commit_msg = "loopengine: trigger CI"
        commit_proc = await asyncio.to_thread(
            subprocess.run,
            ["git", "commit", "--allow-empty", "-m", commit_msg],
            cwd=self.chaos_engine_dir,
            capture_output=True,
            text=True,
            timeout=30,
        )
        if commit_proc.returncode != 0:
            raise RuntimeError(
                f"git commit --allow-empty 失败 (rc={commit_proc.returncode}): "
                f"{commit_proc.stderr.strip()}"
            )
        logger.debug("[ci] git commit stdout: %s", commit_proc.stdout.strip())

        # 2. git push origin {branch}
        push_proc = await asyncio.to_thread(
            subprocess.run,
            ["git", "push", "origin", branch],
            cwd=self.chaos_engine_dir,
            capture_output=True,
            text=True,
            timeout=60,
        )
        if push_proc.returncode != 0:
            raise RuntimeError(
                f"git push origin {branch} 失败 (rc={push_proc.returncode}): "
                f"{push_proc.stderr.strip()}"
            )
        logger.debug("[ci] git push stdout: %s", push_proc.stdout.strip())

        # 3. 获取新 commit 的 sha
        sha_proc = await asyncio.to_thread(
            subprocess.run,
            ["git", "rev-parse", "HEAD"],
            cwd=self.chaos_engine_dir,
            capture_output=True,
            text=True,
            timeout=10,
        )
        if sha_proc.returncode != 0:
            raise RuntimeError(
                f"git rev-parse HEAD 失败 (rc={sha_proc.returncode}): "
                f"{sha_proc.stderr.strip()}"
            )
        commit_sha = sha_proc.stdout.strip()

        logger.info(
            "[ci] CI 已通过 git push 触发, branch=%s, commit_sha=%s",
            branch, commit_sha[:12],
        )

        return {
            "triggered": True,
            "branch": branch,
            "commit_sha": commit_sha,
            "method": "git_push",
        }

    async def get_pipeline_status(self, commit_sha: str) -> dict:
        """查询 CI 构建状态 - 通过 Gitee API 查 commit 的构建状态。

        Gitee 免费版 commit status API（GET /repos/{repo}/commits/{sha}/status）
        也不可用（404）。尝试以下替代方案：

        1. GET /repos/{repo}/builds?sha={sha} - 查指定 sha 的构建记录
        2. 回退：GET /repos/{repo}/commits?per_page=1 - 查最新 commit 的状态字段

        Args:
            commit_sha: 触发 CI 的 commit SHA。

        Returns:
            dict: 状态信息，格式为:
                {
                    "status": "running" | "success" | "failure" | "unknown",
                    "sha": str,
                    "jobs": [{"name": str, "status": str, "conclusion": str|None}]
                }
        """
        # 方案1: GET /repos/{repo}/builds?sha={sha}
        builds_url = f"{self.api_base}/repos/{self.repo}/builds"
        try:
            async with httpx.AsyncClient(timeout=15) as client:
                resp = await client.get(
                    builds_url,
                    headers=self._headers(),
                    params={**self._params(), "sha": commit_sha},
                )
        except httpx.HTTPError as e:
            logger.warning("[ci] 查询 builds API 网络异常: %s", e)
            return {"status": "unknown", "sha": commit_sha, "jobs": []}

        if resp.status_code == 200:
            data = resp.json()
            return self._parse_builds_response(data, commit_sha)

        # 方案2回退: GET /repos/{repo}/commits/{sha} 查 commit 状态
        commit_url = f"{self.api_base}/repos/{self.repo}/commits/{commit_sha}"
        try:
            async with httpx.AsyncClient(timeout=15) as client:
                resp2 = await client.get(
                    commit_url,
                    headers=self._headers(),
                    params=self._params(),
                )
        except httpx.HTTPError as e:
            logger.warning("[ci] 查询 commit API 网络异常: %s", e)
            return {"status": "unknown", "sha": commit_sha, "jobs": []}

        if resp2.status_code == 200:
            commit_data = resp2.json()
            # Gitee commit 可能有 build_status / ci_status 字段
            status = (
                commit_data.get("build_status")
                or commit_data.get("ci_status")
                or commit_data.get("status")
                or "unknown"
            )
            return {
                "status": self._normalize_status(status),
                "sha": commit_sha,
                "jobs": [],
            }

        logger.warning(
            "[ci] 查询 CI 状态失败: builds HTTP %d, commits HTTP %d",
            resp.status_code, resp2.status_code,
        )
        return {"status": "unknown", "sha": commit_sha, "jobs": []}

    def _parse_builds_response(self, data: dict | list, commit_sha: str) -> dict:
        """解析 GET /builds 响应。

        Gitee builds API 可能返回列表或含列表的 dict。
        """
        builds: list = []
        if isinstance(data, list):
            builds = data
        elif isinstance(data, dict):
            builds = data.get("data") or data.get("builds") or data.get("items") or []
            if isinstance(builds, dict):
                builds = [builds]

        if not builds:
            return {"status": "unknown", "sha": commit_sha, "jobs": []}

        # 取第一个（最新）构建记录
        build = builds[0] if isinstance(builds, list) else {}
        status = (
            build.get("status")
            or build.get("state")
            or build.get("result")
            or "unknown"
        )

        jobs = []
        jobs_raw = build.get("jobs") or build.get("stages") or []
        for job in jobs_raw:
            jobs.append({
                "name": job.get("name") or job.get("job_name") or "",
                "status": job.get("status") or job.get("state") or "",
                "conclusion": job.get("conclusion") or job.get("result") or None,
            })

        return {
            "status": self._normalize_status(status),
            "sha": commit_sha,
            "jobs": jobs,
        }

    def _normalize_status(self, raw: str) -> str:
        """将 Gitee 各种状态值归一化。"""
        if not raw:
            return "unknown"
        raw_lower = raw.lower()

        # 运行中
        if raw_lower in self.RUNNING_STATUSES or "run" in raw_lower or "pending" in raw_lower:
            return "running"
        # 成功
        if raw_lower in ("success", "passed", "completed", "succeed", "ok"):
            return "success"
        # 失败
        if raw_lower in ("failure", "failed", "error", "cancelled", "canceled"):
            return "failure"
        # 跳过
        if raw_lower in ("skipped", "skip"):
            return "skipped"
        return "unknown"

    async def poll_until_complete(
        self,
        trigger_result: dict,
        timeout: int = 1800,
        interval: int = 30,
    ) -> dict:
        """轮询 CI 构建状态直到完成或超时。

        Args:
            trigger_result: trigger_pipeline 的返回值，需包含 commit_sha。
            timeout: 最大等待时间（秒），默认 1800（30 分钟）。
            interval: 轮询间隔（秒），默认 30。

        Returns:
            dict: 最终的 CI 状态（含 jobs），格式同 get_pipeline_status。
                  超时则 status 设为 "timeout"。
        """
        commit_sha = trigger_result.get("commit_sha", "")
        if not commit_sha:
            return {"status": "unknown", "sha": "", "jobs": [], "error": "无 commit_sha"}

        start = time.time()
        last_status: dict = {"status": "unknown", "sha": commit_sha, "jobs": []}

        # 首次等待一个 interval，给 Gitee Go 启动时间
        await asyncio.sleep(min(interval, 10))

        while time.time() - start < timeout:
            try:
                status_info = await self.get_pipeline_status(commit_sha)
                last_status = status_info
                status = status_info["status"]

                logger.debug(
                    "[ci] 轮询 CI 状态: status=%s, elapsed=%.0fs",
                    status, time.time() - start,
                )

                if status in self.TERMINAL_STATUSES:
                    return status_info

            except Exception as e:
                logger.warning("[ci] 轮询异常（继续重试）: %s", e)

            await asyncio.sleep(interval)

        # 超时
        last_status["status"] = "timeout"
        return last_status

    async def fetch_job_logs(self, commit_sha: str, job_name: str) -> str:
        """拉取指定 job 的日志。

        Gitee 免费版可能无法通过 API 获取 CI job 日志。
        尝试通过 builds API 查找 job_id 再拉日志，失败则返回提示信息。

        Args:
            commit_sha: 触发 CI 的 commit SHA。
            job_name: job 名称（如 "build-and-test"）。

        Returns:
            str: job 日志文本。拉取失败时返回提示信息字符串。
        """
        status_info = await self.get_pipeline_status(commit_sha)
        jobs = status_info.get("jobs", [])

        job_id = None
        for job in jobs:
            name = job.get("name", "")
            if job_name in name or name in job_name:
                job_id = str(job.get("id") or job.get("job_id") or "")
                break

        if not job_id:
            return (
                f"[Gitee 免费版限制] 无法通过 API 获取 job '{job_name}' 的日志。"
                f"请前往 https://gitee.com/{self.repo}/builds 手动查看。"
            )

        url = f"{self.api_base}/repos/{self.repo}/builds/jobs/{job_id}/logs"
        try:
            async with httpx.AsyncClient(timeout=30) as client:
                resp = await client.get(
                    url, headers=self._headers(), params=self._params(),
                )
        except httpx.HTTPError:
            return (
                f"[Gitee 免费版限制] 拉取 job '{job_name}' 日志网络异常。"
                f"请前往 https://gitee.com/{self.repo}/builds 手动查看。"
            )

        if resp.status_code >= 400:
            return (
                f"[Gitee 免费版限制] 拉取 job '{job_name}' 日志失败 (HTTP {resp.status_code})。"
                f"请前往 https://gitee.com/{self.repo}/builds 手动查看。"
            )

        return resp.text

    async def parse_test_results(self, trigger_result: dict) -> dict:
        """解析所有 job 的测试结果 - 基于 CI 构建状态。

        Gitee 免费版无法通过 API 获取 CI job 日志，因此解析能力有限。
        基于 get_pipeline_status 返回的整体构建状态推断各 job 结果。

        限制说明：
        - 无法获取 ctest 通过/失败数（需解析 job 日志，但 API 不可用）
        - 无法获取 lua-lint 错误数
        - 无法获取 valgrind 泄漏数
        - 只能基于整体 CI 状态推断 success/failure

        Args:
            trigger_result: trigger_pipeline 的返回值，需包含 commit_sha。

        Returns:
            dict: 解析结果，格式为:
                {
                    "build_test": {"status": str, "ctest_pass": int, "ctest_fail": int},
                    "lua_lint": {"status": str, "errors": int},
                    "valgrind": {"status": str, "leaks": int},
                    "smoke": {"status": str},
                    "note": str  # 说明限制
                }
        """
        commit_sha = trigger_result.get("commit_sha", "")
        if not commit_sha:
            return {
                "build_test": {"status": "unknown", "ctest_pass": 0, "ctest_fail": 0},
                "lua_lint": {"status": "unknown", "errors": 0},
                "valgrind": {"status": "unknown", "leaks": 0},
                "smoke": {"status": "unknown"},
                "note": "无 commit_sha，无法查询 CI 状态",
            }

        status_info = await self.get_pipeline_status(commit_sha)
        ci_status = status_info.get("status", "unknown")
        jobs = status_info.get("jobs", [])

        # 如果有 job 级别状态，按 job 解析
        job_status_map: dict[str, str] = {}
        for job in jobs:
            name = job.get("name", "")
            status = job.get("conclusion") or job.get("status") or "unknown"
            job_status_map[name] = status

        # 如果有 job 级别信息，使用 job 级别；否则用整体 CI 状态
        if job_status_map:
            bt_status = _find_job_status(job_status_map, "build-and-test", "build_test")
            lint_status = _find_job_status(job_status_map, "lua-lint", "lua_lint")
            vg_status = _find_job_status(
                job_status_map, "memcheck", "valgrind", "memcheck"
            )
            smoke_status = bt_status  # 冒烟在 build-and-test 内
        else:
            # 无 job 级别信息，整体 CI 状态映射到所有 job
            bt_status = ci_status
            lint_status = ci_status
            vg_status = ci_status
            smoke_status = ci_status

        # ctest/lint/valgrind 的详细数值无法获取（需日志解析，API 不可用）
        results: dict = {
            "build_test": {
                "status": bt_status,
                "ctest_pass": 0,  # 无法获取，需人工确认
                "ctest_fail": 0,
            },
            "lua_lint": {
                "status": lint_status,
                "errors": 0,  # 无法获取，需人工确认
            },
            "valgrind": {
                "status": vg_status,
                "leaks": 0,  # 无法获取，需人工确认
            },
            "smoke": {
                "status": smoke_status,
            },
            "note": (
                "Gitee 免费版无法通过 API 获取 CI job 日志，"
                "ctest/lua-lint/valgrind 的详细数值无法自动解析，需人工确认。"
                f"CI 整体状态: {ci_status}。"
                f"查看详情: https://gitee.com/{self.repo}/builds"
            ),
        }

        return results


# ── 日志解析辅助函数（保留，用于未来 API 可用时） ──

# ctest 输出格式: "100% tests passed, 0 tests failed out of 18"
_CTEST_PATTERN = re.compile(
    r"(\d+)%\s*tests\s*passed,\s*(\d+)\s*tests\s*failed\s*out\s*of\s*(\d+)",
    re.IGNORECASE,
)

# Lua lint 输出格式: "❌ 语法错误: xxx.lua"
_LUA_ERROR_PATTERN = re.compile(r"语法错误|syntax\s+error", re.IGNORECASE)

# Valgrind 输出格式: "definitely lost: 12 bytes in 1 blocks"
_VALGRIND_LEAK_PATTERN = re.compile(
    r"definitely\s+lost:\s*([0-9,]+)\s+bytes\s+in\s+(\d+)\s+blocks",
    re.IGNORECASE,
)


def _parse_ctest_summary(log_text: str) -> tuple[int, int]:
    """从日志解析 ctest 通过/失败数。

    匹配 "X% tests passed, Y tests failed out of Z" 格式。

    Returns:
        (pass_count, fail_count)，未匹配到返回 (0, 0)。
    """
    match = _CTEST_PATTERN.search(log_text)
    if not match:
        return (0, 0)

    fail_count = int(match.group(2))
    total = int(match.group(3))
    pass_count = total - fail_count
    return (pass_count, fail_count)


def _parse_lua_lint_errors(log_text: str) -> int:
    """从日志解析 Lua 语法错误数。

    Returns:
        错误行数，未匹配到返回 0。
    """
    count = 0
    for line in log_text.splitlines():
        if _LUA_ERROR_PATTERN.search(line):
            count += 1
    return count


def _parse_valgrind_leaks(log_text: str) -> int:
    """从日志解析 Valgrind 内存泄漏数。

    Returns:
        泄漏 block 数，未匹配到返回 0。
    """
    total_blocks = 0
    for match in _VALGRIND_LEAK_PATTERN.finditer(log_text):
        total_blocks += int(match.group(2))
    return total_blocks


def _find_job_status(
    job_map: dict[str, str], *name_candidates: str
) -> str:
    """从 job 状态映射中按名称候选查找状态。

    Args:
        job_map: {job_name: status} 映射。
        name_candidates: 候选 job 名称（按优先级）。

    Returns:
        匹配到的状态，未找到返回 "unknown"。
    """
    for name in job_map:
        name_lower = name.lower()
        for candidate in name_candidates:
            if candidate.lower() in name_lower:
                return job_map[name]

    # 也检查候选名是否直接是 key
    for candidate in name_candidates:
        if candidate in job_map:
            return job_map[candidate]

    return "unknown"
