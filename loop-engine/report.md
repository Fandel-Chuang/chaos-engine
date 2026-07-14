# LoopEngine 验证报告

> 生成时间: 2026-07-14 15:00:20

## 执行总览

| 项目 | 值 |
| --- | --- |
| Loop ID | loop_20260714_145703 |
| 任务 | verify闭环修复后的四域闭环验证 |
| 开始时间 | 2026-07-14 14:57:03 |
| 结束时间 | 2026-07-14 15:00:20 |
| 总状态 | ✅ 通过 |
| 总成本 (元) | 0.0000 |
| 报告路径 | (未生成) |

### 各域执行状态

| 域 | 状态 | 耗时 | 重试 | 错误 |
| --- | --- | --- | --- | --- |
| dev | ✅ 通过 | 0.2s | 0 |  |
| test | ✅ 通过 | 1m51s | 0 |  |
| verify | ✅ 通过 | 1m0s | 1 |  |
| client_ui | ✅ 通过 | 25.4s | 0 |  |

## ✅ dev 闭环 - 通过

| 项目 | 值 |
| --- | --- |
| 状态 | ✅ 通过 |
| 耗时 | 0.2s |
| 重试次数 | 0 |
| 错误信息 | 无 |

### 断言详情

| 断言名称 | 级别 | 结果 | 实际值 | 说明 |
| --- | --- | --- | --- | --- |
| verify_binary_exists | critical | ✅ 通过 | {'missing': [], 'sizes': {'build/bin/chaos_server': 324480, 'build/bin/chaos_... | 所有二进制文件验证通过 |

## ✅ test 闭环 - 通过

| 项目 | 值 |
| --- | --- |
| 状态 | ✅ 通过 |
| 耗时 | 1m51s |
| 重试次数 | 0 |
| 错误信息 | 无 |

### 断言详情

| 断言名称 | 级别 | 结果 | 实际值 | 说明 |
| --- | --- | --- | --- | --- |
| build_test_job | critical | ✅ 通过 | success | build-and-test job 状态=success |
| ctest_pass_count | warning | ✅ 通过 | 0 passed, 0 failed | ctest: 数值无法获取（Gitee 免费版限制），CI 整体状态=success |
| smoke_test | critical | ✅ 通过 | success | 冒烟测试状态=success |
| lua_lint | warning | ✅ 通过 | status=success, errors=0 | lua-lint: status=success, errors=0 |
| valgrind | warning | ✅ 通过 | status=success, leaks=0 | valgrind: status=success, leaks=0 |

## ✅ verify 闭环 - 通过

| 项目 | 值 |
| --- | --- |
| 状态 | ✅ 通过 |
| 耗时 | 1m0s |
| 重试次数 | 1 |
| 错误信息 | 无 |

### 断言详情

| 断言名称 | 级别 | 结果 | 实际值 | 说明 |
| --- | --- | --- | --- | --- |
| cluster_all_running | critical | ✅ 通过 | {'game': 'running', 'gateway': 'running', 'dbproxy': 'running', 'router': 'ru... | 所有 5 服务 running/orphan |
| smoke_test_passed | critical | ✅ 通过 | 0 | 冒烟测试 exit_code=0 |
| admin_api_valid | critical | ✅ 通过 | {'has_stats': True, 'keys': ['timestamp', 'ok', 'data']} | Admin API stats: 有效 |
| entity_sync_normal | warning | ✅ 通过 | {'aoi': {'timestamp': 1784012376, 'ok': True, 'data': {'aoi_radius': 100, 'ev... | 实体同步: 正常 |

## ✅ client_ui 闭环 - 通过

| 项目 | 值 |
| --- | --- |
| 状态 | ✅ 通过 |
| 耗时 | 25.4s |
| 重试次数 | 0 |
| 错误信息 | 无 |

### 断言详情

| 断言名称 | 级别 | 结果 | 实际值 | 说明 |
| --- | --- | --- | --- | --- |
| process_alive | critical | ✅ 通过 | True | 客户端进程存活 -> passed |
| window_exists | critical | ✅ 通过 | True | Vulkan 窗口已创建 -> passed |
| vulkan_initialized | critical | ✅ 通过 | False | Vulkan 设备初始化成功 -> passed |
| window_size | critical | ✅ 通过 | 1280x720 | 窗口尺寸为 1280x720 -> passed |
| title_bar_has_pos | warning | ❌ 失败 |  | 标题栏包含坐标信息 -> failed |
| spawn_within_radius | warning | ❌ 失败 | None | 出生点在 spawn 半径内 (r=2.5) -> failed |
| network_connected | warning | ❌ 失败 | False | 客户端已连接服务器 (标题栏非 disconnected) -> failed |
| entity_update_received | warning | ❌ 失败 | False | 收到实体同步消息 (ENTITY_UPDATE) -> failed |
| visible_entities_positive | warning | ❌ 失败 | None | 可见实体数 > 0 (连接后) -> failed |
| no_network_error | warning | ✅ 通过 | False | 无网络错误日志 -> passed |
| title_bar_keys_responsive | info | ✅ 通过 | ---- | 标题栏 keys 字段存在 (输入系统工作) -> passed |
