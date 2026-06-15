/*
 * ChaosEngine 编辑器 UI 面板接口
 * 纯终端 UI 实现（无 Dear ImGui 依赖）
 */

#pragma once

namespace ChaosEditor {

    /** 初始化 UI 子系统 */
    void Init();

    /** 关闭 UI 子系统 */
    void Shutdown();

    /** 每帧开始（清屏、重置布局） */
    void BeginFrame();

    /** 每帧结束（刷新输出） */
    void EndFrame();

    /** 实体层次结构面板 */
    void ShowHierarchy();

    /** 选中实体属性面板（Inspector） */
    void ShowInspector();

    /** 日志控制台面板 */
    void ShowConsole();

    /** 插件状态监控面板 */
    void ShowPluginMonitor();

    /** 引擎统计面板 */
    void ShowStats();

} // namespace ChaosEditor
