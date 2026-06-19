#!/bin/bash
set -euo pipefail
DEFAULT_MODEL="qwen3:4b"
TARGET_MODEL="${1:-$DEFAULT_MODEL}"
OLLAMA_BIN_DIR="/usr/local/bin"
OLLAMA_LIB_DIR="/usr/local/lib/ollama"
TMP_ZIP="$HOME/tmp_ollama.tar.zst"

# 1. 判断ollama是否存在，不存在离线安装兼容老卡版本
if ! command -v ollama &> /dev/null; then
    echo "====================="
    echo "未检测Ollama，离线安装兼容GTX1060的0.1.40稳定版"
    echo "====================="
    sudo rm -rf "$OLLAMA_LIB_DIR" "$OLLAMA_BIN_DIR/ollama"
    # 国内直链下载固定旧版二进制，规避代理失败
    wget -c https://mirror.tuna.tsinghua.edu.cn/github-release/ollama/ollama/LATEST/ollama-linux-amd64.tar.zst -O "$TMP_ZIP"
    sudo tar -C /usr/local -xzf "$TMP_ZIP"
    rm -f "$TMP_ZIP"
    # 修复全局执行权限
    sudo chmod 755 "$OLLAMA_BIN_DIR/ollama"
    # 刷新系统环境变量
    source /etc/profile
    echo "Ollama 安装完成，版本：$(ollama --version)"
fi

# 2. 配置模型国内魔搭镜像，持久写入bashrc
MIRROR_CFG='export OLLAMA_HF_MIRROR=https://modelscope.cn'
if ! grep -qxF "$MIRROR_CFG" ~/.bashrc; then
    echo "$MIRROR_CFG" >> ~/.bashrc
    echo "已写入模型加速镜像 ~/.bashrc"
fi
export OLLAMA_HF_MIRROR=https://modelscope.cn

# 3. 拉取目标模型
echo -e "\n===== 开始下载模型：$TARGET_MODEL ====="
ollama pull "$TARGET_MODEL"

echo -e "\n✅ 下载完成！GTX1060 6G防显存溢出启动命令："
echo "OLLAMA_KV_CACHE_TYPE=q4_0 ollama run $TARGET_MODEL"
