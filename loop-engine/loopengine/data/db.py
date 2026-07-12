"""SQLite 持久化层 - 异步接口。"""

from __future__ import annotations

import json
import os
from datetime import datetime
from pathlib import Path
from typing import Any

import aiosqlite

from .models import LoopExecution, LoopStatus

# 默认数据库路径：固定在 ~/.loopengine/ 目录下，避免因 cwd 不同导致数据库分散
_DEFAULT_DB_PATH = os.path.join(os.path.expanduser("~"), ".loopengine", "loopengine.db")

_SCHEMA = """
CREATE TABLE IF NOT EXISTS loop_executions (
    loop_id         TEXT PRIMARY KEY,
    task            TEXT,
    started_at      TEXT,
    finished_at     TEXT,
    status          TEXT,
    domains         TEXT,
    total_cost_cny  REAL,
    report_path     TEXT
);
"""


def _exec_to_row(exec: LoopExecution) -> dict[str, Any]:
    """将 LoopExecution 转成数据库行字典。"""
    return {
        "loop_id": exec.loop_id,
        "task": exec.task,
        "started_at": exec.started_at.isoformat() if exec.started_at else None,
        "finished_at": exec.finished_at.isoformat() if exec.finished_at else None,
        "status": exec.status.value,
        "domains": json.dumps(exec.domains, ensure_ascii=False),
        "total_cost_cny": exec.total_cost_cny,
        "report_path": exec.report_path,
    }


def _row_to_exec(row: dict[str, Any]) -> LoopExecution:
    """数据库行字典转 LoopExecution。"""
    return LoopExecution(
        loop_id=row["loop_id"],
        task=row["task"],
        started_at=datetime.fromisoformat(row["started_at"]) if row["started_at"] else datetime.now(),
        finished_at=datetime.fromisoformat(row["finished_at"]) if row["finished_at"] else None,
        status=LoopStatus(row["status"]) if row["status"] else LoopStatus.PENDING,
        domains=json.loads(row["domains"]) if row["domains"] else [],
        total_cost_cny=row["total_cost_cny"] or 0.0,
        report_path=row["report_path"] or "",
    )


async def init_db(db_path: str = _DEFAULT_DB_PATH) -> None:
    """初始化数据库，建表。"""
    Path(db_path).parent.mkdir(parents=True, exist_ok=True)
    async with aiosqlite.connect(db_path) as db:
        await db.execute(_SCHEMA)
        await db.commit()


async def save_execution(exec: LoopExecution, db_path: str = _DEFAULT_DB_PATH) -> None:
    """保存或更新一条执行记录。"""
    row = _exec_to_row(exec)
    async with aiosqlite.connect(db_path) as db:
        await db.execute(
            """INSERT OR REPLACE INTO loop_executions
               (loop_id, task, started_at, finished_at, status, domains, total_cost_cny, report_path)
               VALUES (:loop_id, :task, :started_at, :finished_at, :status, :domains, :total_cost_cny, :report_path)""",
            row,
        )
        await db.commit()


async def get_execution(loop_id: str, db_path: str = _DEFAULT_DB_PATH) -> LoopExecution | None:
    """按 loop_id 查询单条执行记录。"""
    async with aiosqlite.connect(db_path) as db:
        db.row_factory = aiosqlite.Row
        cursor = await db.execute(
            "SELECT * FROM loop_executions WHERE loop_id = ?", (loop_id,)
        )
        row = await cursor.fetchone()
        if row is None:
            return None
        return _row_to_exec(dict(row))


async def list_executions(limit: int = 20, db_path: str = _DEFAULT_DB_PATH) -> list[LoopExecution]:
    """列出最近的执行记录。"""
    async with aiosqlite.connect(db_path) as db:
        db.row_factory = aiosqlite.Row
        cursor = await db.execute(
            "SELECT * FROM loop_executions ORDER BY started_at DESC LIMIT ?", (limit,)
        )
        rows = await cursor.fetchall()
        return [_row_to_exec(dict(r)) for r in rows]
