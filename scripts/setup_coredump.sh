#!/bin/bash
# ChaosEngine coredump 捕获设置脚本
# 用法: ./scripts/setup_coredump.sh
#   启动: 开启 coredump 捕获，存到 .ci-reports/coredumps/
#   停止: 关闭 coredump 捕获，清理设置
#   清理: 删除已修复的 coredump 文件

CORE_DIR="/home/zhongfangdao/chaos-engine/.ci-reports/coredumps"
ACTION="${1:-start}"

case "$ACTION" in
    start)
        mkdir -p "$CORE_DIR"
        # 1. 取消 core 大小限制
        ulimit -c unlimited 2>/dev/null
        # 2. 设置 core_pattern 写入指定目录，文件名含进程名+PID+时间+信号
        echo "$CORE_DIR/core.%e.%p.%t.%s" | sudo tee /proc/sys/kernel/core_pattern
        # 3. core 文件名加 PID
        sudo sysctl -w kernel.core_uses_pid=1 >/dev/null 2>&1
        # 4. apport 会拦截 core，临时停掉
        sudo systemctl stop apport 2>/dev/null
        echo "✅ coredump 捕获已开启"
        echo "   存储目录: $CORE_DIR"
        echo "   文件格式: core.<程序名>.<PID>.<时间戳>.<信号>"
        echo ""
        echo "当前设置:"
        echo "  ulimit -c: $(ulimit -c)"
        echo "  core_pattern: $(cat /proc/sys/kernel/core_pattern)"
        echo "  core_uses_pid: $(cat /proc/sys/kernel/core_uses_pid 2>/dev/null || sysctl -n kernel.core_uses_pid)"
        ;;

    stop)
        echo "|/usr/share/apport/apport -p%p -s%s -c%c -d%d -P%P -u%u -g%g -F%F -- %E" | sudo tee /proc/sys/kernel/core_pattern
        sudo systemctl start apport 2>/dev/null
        ulimit -c 0 2>/dev/null
        echo "✅ coredump 捕获已关闭，恢复 apport"
        ;;

    clean)
        count=$(ls -1 "$CORE_DIR"/core.* 2>/dev/null | wc -l)
        if [ "$count" -eq 0 ]; then
            echo "没有 coredump 文件需要清理"
        else
            rm -f "$CORE_DIR"/core.*
            echo "✅ 已清理 $count 个 coredump 文件"
        fi
        ;;

    list)
        echo "=== coredump 文件列表 ==="
        if ls "$CORE_DIR"/core.* >/dev/null 2>&1; then
            ls -lhS "$CORE_DIR"/core.* | awk '{printf "  %s  %s\n", $5, $NF}'
        else
            echo "  (无)"
        fi
        ;;

    *)
        echo "用法: $0 {start|stop|clean|list}"
        echo "  start  开启 coredump 捕获"
        echo "  stop   关闭 coredump 捕获"
        echo "  clean  清理已修复的 coredump 文件"
        echo "  list   列出当前 coredump 文件"
        ;;
esac
