#!/bin/bash
# 需要 export GITHUB_TOKEN 环境变量
cd /home/zhongfangdao/chaos-engine/loop-engine
python3 -m loopengine.cli run --config loop.yaml --report /home/zhongfangdao/chaos-engine/loop-engine/report.md --task "loopengine full cycle" 2>&1
