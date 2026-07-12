"""CI 触发器 - spec 6.2.2。

通过 Gitee Go API 触发/轮询/解析 CI 流水线。
核心原则：测试流程必须通过 Git CI 完成，LoopEngine 只触发/轮询/解析，不本地执行测试脚本。
复用 .gitee-ci.yml 现有 5 job（build-and-test/release-build/lua-lint/memcheck），
不新增不修改。
"""

from __future__ import annotations

import asyncio
import re
import time

import httpx


class CITrigger:
    """Gitee Go CI 触发器。

    通过 Gitee API v5 触发流水线、查询状态、拉取日志、解析测试结果。
    """

    API_BASE = "https://gitee.com/api/v5"

    # CI 终态状态值
    TERMINAL_STATUSES = {"completed", "failure", "success", "cancelled", "skipped"}

    def __init__(
        self,
        gitee_token: str,
        repo: str = "zhong-fangdao/chaos-engine",
    ) -> None:
        """初始化 CI 触发器。

        Args:
            gitee_token: Gitee API 访问令牌。
            repo: 仓库全名（owner/repo）。
        """
        self.gitee_token = gitee_token
        self.repo = repo
        self.api_base = self.API_BASE

    def _headers(self) -> dict[str, str]:
        """构建请求头。"""
        return {
            "Authorization": f"token {self.gitee_token}",
            "Content-Type": "application/json",
            "Accept": "application/json",
        }

    def _runs_url(self) -> str:
        """流水线 runs 基础 URL。"""
        return f"{self.api_base}/repos/{self.repo}/actions/runs"

    async def trigger_pipeline(self, branch: str = "develop") -> dict:
        """触发 CI 流水线。

        通过 Gitee API v5 触发指定分支的 Actions 流水线。

        Args:
            branch: 触发分支名。

        Returns:
            dict: 流水线信息，至少包含 run_id。

        Raises:
            RuntimeError: 触发失败（HTTP 非 2xx）。
        """
        url = self._runs_url()
        # Gitee API v5 要求 access_token 查询参数认证
        payload = {"branch": branch}

        async with httpx.AsyncClient(timeout=30) as client:
            resp = await client.post(
                url, json=payload, headers=self._headers(),
                params={"access_token": self.gitee_token},
            )

        if resp.status_code >= 400:
            raise RuntimeError(
                f"触发流水线失败 (HTTP {resp.status_code}): {resp.text}"
            )

        data = resp.json()
        # Gitee 返回的 run id 字段可能是 id 或 run_id
        run_id = str(data.get("id") or data.get("run_id") or "")
        return {"run_id": run_id, "branch": branch, "raw": data}

    async def get_pipeline_status(self, run_id: str) -> dict:
        """查询流水线状态。

        Args:
            run_id: 流水线运行 ID。

        Returns:
            dict: 状态信息，格式为:
                {
                    "status": "running" | "completed" | "failure" | ...,
                    "jobs": [{"name": str, "status": str, "conclusion": str|None}]
                }

        Raises:
            RuntimeError: 查询失败。
        """
        url = f"{self._runs_url()}/{run_id}"

        async with httpx.AsyncClient(timeout=15) as client:
            resp = await client.get(
                url, headers=self._headers(),
                params={"access_token": self.gitee_token},
            )

        if resp.status_code >= 400:
            raise RuntimeError(
                f"查询流水线状态失败 (HTTP {resp.status_code}): {resp.text}"
            )

        data = resp.json()

        # 解析流水线总体状态
        status = data.get("status") or data.get("state") or "unknown"

        # 解析 jobs 列表
        jobs_raw = data.get("jobs") or data.get("workflow_run_jobs") or []
        jobs = []
        for job in jobs_raw:
            jobs.append(
                {
                    "name": job.get("name") or job.get("job_name") or "",
                    "status": job.get("status") or job.get("state") or "",
                    "conclusion": job.get("conclusion")
                    or job.get("result")
                    or None,
                }
            )

        return {"status": status, "jobs": jobs, "raw": data}

    async def poll_until_complete(
        self,
        run_id: str,
        timeout: int = 1800,
        interval: int = 15,
    ) -> dict:
        """轮询流水线直到完成或超时。

        Args:
            run_id: 流水线运行 ID。
            timeout: 最大等待时间（秒）。
            interval: 轮询间隔（秒）。

        Returns:
            dict: 最终的流水线状态（含 jobs）。
        """
        start = time.time()
        while time.time() - start < timeout:
            status_info = await self.get_pipeline_status(run_id)
            status = status_info["status"]

            if status in self.TERMINAL_STATUSES:
                return status_info

            await asyncio.sleep(interval)

        # 超时，返回最后一次状态并标记
        last_status = await self.get_pipeline_status(run_id)
        last_status["status"] = "timeout"
        return last_status

    async def fetch_job_logs(self, run_id: str, job_name: str) -> str:
        """拉取指定 job 的日志。

        先查询流水线获取 job_id，再拉取日志内容。

        Args:
            run_id: 流水线运行 ID。
            job_name: job 名称（如 "build-and-test"）。

        Returns:
            str: job 日志文本。拉取失败时返回空字符串。
        """
        # 先获取 job 列表，找到 job_id
        status_info = await self.get_pipeline_status(run_id)
        jobs = status_info.get("raw", {}).get("jobs") or []

        job_id = None
        for job in jobs:
            name = job.get("name") or job.get("job_name") or ""
            if job_name in name or name in job_name:
                job_id = str(job.get("id") or job.get("job_id") or "")
                break

        if not job_id:
            return ""

        url = f"{self._runs_url()}/{run_id}/jobs/{job_id}/logs"

        async with httpx.AsyncClient(timeout=30) as client:
            resp = await client.get(
                url, headers=self._headers(),
                params={"access_token": self.gitee_token},
            )

        if resp.status_code >= 400:
            return ""

        return resp.text

    async def parse_test_results(self, run_id: str) -> dict:
        """解析所有 job 的测试结果。

        拉取各 job 日志，解析结构化结果:
        - build_test: ctest 通过/失败数
        - lua_lint: 语法错误数
        - valgrind: 内存泄漏数
        - smoke: 集群冒烟结果

        Args:
            run_id: 流水线运行 ID。

        Returns:
            dict: 解析结果，格式为:
                {
                    "build_test": {"status": str, "ctest_pass": int, "ctest_fail": int},
                    "lua_lint": {"status": str, "errors": int},
                    "valgrind": {"status": str, "leaks": int},
                    "smoke": {"status": str}
                }
        """
        # 获取流水线状态和 job 列表
        status_info = await self.get_pipeline_status(run_id)
        jobs = status_info.get("jobs", [])

        # job 名称到结果的映射
        job_status_map: dict[str, str] = {}
        for job in jobs:
            name = job.get("name", "")
            # 归一化 job 名
            status = job.get("conclusion") or job.get("status") or "unknown"
            job_status_map[name] = status

        results: dict = {}

        # ── build_test: 解析 ctest 结果 ──
        bt_status = _find_job_status(job_status_map, "build-and-test", "build_test")
        bt_log = await self._safe_fetch_log(run_id, "build-and-test")
        ctest_pass, ctest_fail = _parse_ctest_summary(bt_log)
        results["build_test"] = {
            "status": bt_status,
            "ctest_pass": ctest_pass,
            "ctest_fail": ctest_fail,
        }

        # ── lua_lint: 解析语法错误数 ──
        lint_status = _find_job_status(job_status_map, "lua-lint", "lua_lint")
        lint_log = await self._safe_fetch_log(run_id, "lua-lint")
        lint_errors = _parse_lua_lint_errors(lint_log)
        results["lua_lint"] = {
            "status": lint_status,
            "errors": lint_errors,
        }

        # ── valgrind: 解析内存泄漏数 ──
        vg_status = _find_job_status(
            job_status_map, "memcheck", "valgrind", "memcheck"
        )
        vg_log = await self._safe_fetch_log(run_id, "memcheck")
        vg_leaks = _parse_valgrind_leaks(vg_log)
        results["valgrind"] = {
            "status": vg_status,
            "leaks": vg_leaks,
        }

        # ── smoke: 集群冒烟（build-and-test job 中的冒烟步骤） ──
        results["smoke"] = {
            "status": bt_status,  # 冒烟测试在 build-and-test job 内
        }

        return results

    async def _safe_fetch_log(self, run_id: str, job_name: str) -> str:
        """安全拉取日志，失败时返回空字符串。"""
        try:
            return await self.fetch_job_logs(run_id, job_name)
        except Exception:
            return ""


# ── 日志解析辅助函数 ──

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
