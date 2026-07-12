#!/bin/bash
# ChaosEngine 客户端 CLI 控制器
# 用法: ./scripts/client_ctl.sh <命令>
#
# 命令列表:
#   fullscreen    切换全屏
#   maximize      最大化窗口
#   minimize      最小化窗口
#   restore       恢复窗口大小
#   resize WxH    调整窗口大小 (如 resize 800x600)
#   focus         聚焦窗口
#   close         关闭客户端
#   move WxH      移动并调整窗口 (如 move 0x0 1920x1080)
#   key <key>     发送按键 (如 key Escape, key F11)
#   mousemove X Y 移动鼠标到坐标
#   click <btn>   鼠标点击 (1=左 2=中 3=右)
#   status        查看客户端窗口状态
#   screenshot    截图到 /tmp/chaos_client_screenshot.png

set -euo pipefail

WINDOW_NAME="ChaosEngine"

find_window() {
    local wid
    wid=$(xdotool search --name "$WINDOW_NAME" 2>/dev/null | head -1)
    if [ -z "$wid" ]; then
        echo "❌ 未找到 ChaosEngine 客户端窗口" >&2
        exit 1
    fi
    echo "$wid"
}

ensure_display() {
    if [ -z "${DISPLAY:-}" ]; then
        export DISPLAY=:0
    fi
    if [ -z "${XAUTHORITY:-}" ]; then
        local auth
        auth=$(pgrep -af Xwayland | grep -oP '(?<=-auth )\S+' | head -1)
        if [ -n "$auth" ] && [ -r "$auth" ]; then
            export XAUTHORITY="$auth"
        fi
    fi
}

cmd=${1:-help}
shift || true

ensure_display

case "$cmd" in
    fullscreen|fs)
        wid=$(find_window)
        # 交替全屏/还原（用 wmctrl toggle）
        wmctrl -i -r "$wid" -b toggle,fullscreen
        echo "✅ 切换全屏 (窗口 $wid)"
        ;;

    maximize|max)
        wid=$(find_window)
        wmctrl -i -r "$wid" -b add,maximized_vert,maximized_horz
        echo "✅ 最大化窗口 ($wid)"
        ;;

    minimize|min)
        wid=$(find_window)
        xdotool windowminimize "$wid"
        echo "✅ 最小化窗口 ($wid)"
        ;;

    restore|normal)
        wid=$(find_window)
        wmctrl -i -r "$wid" -b remove,maximized_vert,maximized_horz,fullscreen
        echo "✅ 恢复窗口 ($wid)"
        ;;

    resize)
        if [ -z "${1:-}" ]; then
            echo "用法: $0 resize WxH (如 resize 800x600)" >&2
            exit 1
        fi
        wid=$(find_window)
        w=$(echo "$1" | cut -dx -f1)
        h=$(echo "$1" | cut -dx -f2)
        wmctrl -i -r "$wid" -e 0,-1,-1,"$w","$h"
        echo "✅ 调整窗口大小: ${w}x${h} ($wid)"
        ;;

    move)
        if [ -z "${1:-}" ] || [ -z "${2:-}" ]; then
            echo "用法: $0 move XxY WxH (如 move 0x0 1920x1080)" >&2
            exit 1
        fi
        wid=$(find_window)
        x=$(echo "$1" | cut -dx -f1)
        y=$(echo "$1" | cut -dx -f2)
        w=$(echo "$2" | cut -dx -f1)
        h=$(echo "$2" | cut -dx -f2)
        wmctrl -i -r "$wid" -e 0,"$x","$y","$w","$h"
        echo "✅ 移动窗口到 ${x},${y} 大小 ${w}x${h} ($wid)"
        ;;

    focus)
        wid=$(find_window)
        xdotool windowactivate "$wid"
        xdotool windowfocus "$wid"
        echo "✅ 聚焦窗口 ($wid)"
        ;;

    close|quit)
        wid=$(find_window)
        xdotool windowclose "$wid"
        echo "✅ 关闭客户端 ($wid)"
        ;;

    key)
        if [ -z "${1:-}" ]; then
            echo "用法: $0 key <key> (如 key Escape, key F11)" >&2
            exit 1
        fi
        wid=$(find_window)
        xdotool windowactivate "$wid" 2>/dev/null || true
        xdotool key --window "$wid" "$1"
        echo "✅ 发送按键: $1 ($wid)"
        ;;

    mousemove)
        if [ -z "${1:-}" ] || [ -z "${2:-}" ]; then
            echo "用法: $0 mousemove X Y" >&2
            exit 1
        fi
        xdotool mousemove "$1" "$2"
        echo "✅ 鼠标移动到 ($1, $2)"
        ;;

    click)
        if [ -z "${1:-}" ]; then
            echo "用法: $0 click <button> (1=左 2=中 3=右)" >&2
            exit 1
        fi
        xdotool click "$1"
        echo "✅ 鼠标点击: 按钮 $1"
        ;;

    status)
        wid=$(find_window)
        echo "=== ChaosEngine 客户端状态 ==="
        echo "窗口 ID: $wid"
        xprop -id "$wid" WM_NAME _NET_WM_STATE 2>/dev/null | head -5
        echo ""
        wmctrl -l -x | grep -i chaos || true
        ;;

    screenshot|shot)
        wid=$(find_window)
        import -window "$wid" /tmp/chaos_client_screenshot.png 2>/dev/null || \
            xdotool key --window "$wid" Print 2>/dev/null || \
            scrot -u /tmp/chaos_client_screenshot.png 2>/dev/null || \
            echo "⚠️ 截图需要 imagemagick(scrot)，尝试: sudo apt install scrot"
        if [ -f /tmp/chaos_client_screenshot.png ]; then
            echo "✅ 截图已保存: /tmp/chaos_client_screenshot.png"
        fi
        ;;

    test-fullscreen)
        # 自动化测试：启动客户端 -> 等待 -> 全屏 -> 等待 -> 还原 -> 等待 -> 关闭
        echo "=== 自动全屏测试 ==="
        wid=$(find_window)
        echo "窗口 ID: $wid"

        echo "[1/5] 等待 2s 让窗口稳定..."
        sleep 2

        echo "[2/5] 切换全屏..."
        wmctrl -i -r "$wid" -b add,fullscreen
        sleep 2

        echo "[3/5] 检查是否 crash..."
        if xdotool getwindowname "$wid" >/dev/null 2>&1; then
            echo "  ✅ 全屏后窗口仍存活"
        else
            echo "  ❌ 全屏后窗口已消失（可能 crash）"
            exit 1
        fi

        echo "[4/5] 退出全屏..."
        wmctrl -i -r "$wid" -b remove,fullscreen
        sleep 1

        echo "[5/5] 检查窗口状态..."
        if xdotool getwindowname "$wid" >/dev/null 2>&1; then
            echo "  ✅ 退出全屏后窗口仍存活"
            echo "=== 测试通过 ==="
        else
            echo "  ❌ 退出全屏后窗口已消失"
            exit 1
        fi
        ;;

    test-resize)
        # 自动化测试：多次 resize 窗口
        echo "=== 自动 resize 测试 ==="
        wid=$(find_window)
        echo "窗口 ID: $wid"

        for size in "800x600" "1920x1080" "640x480" "1280x720" "2560x1440" "320x240"; do
            w=$(echo "$size" | cut -dx -f1)
            h=$(echo "$size" | cut -dx -f2)
            echo "  resize -> ${size}..."
            wmctrl -i -r "$wid" -e 0,-1,-1,"$w","$h"
            sleep 1
            if ! xdotool getwindowname "$wid" >/dev/null 2>&1; then
                echo "  ❌ resize 到 ${size} 后窗口消失"
                exit 1
            fi
        done
        echo "=== resize 测试通过 ==="
        ;;

    help|--help|-h)
        echo "ChaosEngine 客户端 CLI 控制器"
        echo ""
        echo "用法: $0 <命令> [参数]"
        echo ""
        echo "窗口控制:"
        echo "  fullscreen       切换全屏"
        echo "  maximize         最大化窗口"
        echo "  minimize         最小化窗口"
        echo "  restore          恢复窗口大小"
        echo "  resize WxH       调整窗口大小 (如 resize 800x600)"
        echo "  move XxY WxH     移动并调整窗口"
        echo "  focus            聚焦窗口"
        echo "  close            关闭客户端"
        echo ""
        echo "输入模拟:"
        echo "  key <key>        发送按键 (如 key Escape, key F11)"
        echo "  mousemove X Y    移动鼠标到坐标"
        echo "  click <btn>      鼠标点击 (1=左 2=中 3=右)"
        echo ""
        echo "监控:"
        echo "  status           查看窗口状态"
        echo "  screenshot       截图到 /tmp/chaos_client_screenshot.png"
        echo ""
        echo "自动化测试:"
        echo "  test-fullscreen  自动全屏切换测试"
        echo "  test-resize      自动多次 resize 测试"
        ;;

    *)
        echo "未知命令: $cmd" >&2
        echo "用法: $0 help" >&2
        exit 1
        ;;
esac
