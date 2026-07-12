#!/usr/bin/env python3
"""
QQ 邮箱 CI 失败邮件监控脚本
- 每 5 分钟由 cron 调用
- 读取 QQ 邮箱中 CI 失败通知邮件
- 新邮件写入 .ci-reports/qq-mail-alerts/
- 通过 hermes send 推送通知
"""
import imaplib
import email
import os
import sys
import json
import subprocess
from email.header import decode_header
from datetime import datetime

# ============ 配置 ============
QQ_EMAIL = "zfdtc1111@qq.com"
QQ_IMAP_CODE = "onycpyorvduibehj"
IMAP_SERVER = "imap.qq.com"
IMAP_PORT = 993

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPORT_DIR = os.path.join(PROJECT_DIR, ".ci-reports", "qq-mail-alerts")
STATE_FILE = os.path.join(REPORT_DIR, "seen_state.json")

# CI 失败邮件关键词（精确匹配）
CI_KEYWORDS = ['failed', 'failure', '失败', 'error', 'build failed',
               'CI failed', 'workflow', 'Actions run', 'run failed',
               'chaos-engine CI']


def decode_str(raw):
    """解码邮件头"""
    if raw is None:
        return ""
    parts = decode_header(raw)
    result = []
    for data, charset in parts:
        if isinstance(data, bytes):
            result.append(data.decode(charset or 'utf-8', errors='replace'))
        else:
            result.append(data)
    return ''.join(result)


def extract_body(msg):
    """提取邮件正文（纯文本优先）"""
    body = ""
    if msg.is_multipart():
        for part in msg.walk():
            ct = part.get_content_type()
            if ct == 'text/plain':
                payload = part.get_payload(decode=True)
                charset = part.get_content_charset() or 'utf-8'
                body = payload.decode(charset, errors='replace')
                break
            elif ct == 'text/html' and not body:
                payload = part.get_payload(decode=True)
                charset = part.get_content_charset() or 'utf-8'
                body = payload.decode(charset, errors='replace')
    else:
        payload = msg.get_payload(decode=True)
        if payload:
            charset = msg.get_content_charset() or 'utf-8'
            body = payload.decode(charset, errors='replace')
    # 去 HTML 标签
    import re
    body = re.sub(r'<[^>]+>', ' ', body)
    body = re.sub(r'\s+', ' ', body).strip()
    return body[:3000]


def load_seen_state():
    """加载已通知的邮件 ID"""
    if os.path.exists(STATE_FILE):
        try:
            with open(STATE_FILE) as f:
                return json.load(f)
        except Exception:
            pass
    return {"seen_ids": []}


def save_seen_state(state):
    """保存已通知的邮件 ID"""
    os.makedirs(REPORT_DIR, exist_ok=True)
    # 只保留最近 200 条
    if len(state["seen_ids"]) > 200:
        state["seen_ids"] = state["seen_ids"][-200:]
    with open(STATE_FILE, "w") as f:
        json.dump(state, f, indent=2, ensure_ascii=False)


def notify_hermes(subject, body, date_str):
    """通过 hermes send 推送通知"""
    # 写入报告文件
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    report_path = os.path.join(REPORT_DIR, f"alert-{timestamp}.md")
    content = f"""# CI 失败邮件告警

- 时间: {date_str}
- 主题: {subject}

## 邮件内容

{body}
"""
    with open(report_path, "w") as f:
        f.write(content)

    # 更新 latest 汇总
    latest_path = os.path.join(REPORT_DIR, "latest-alert.md")
    with open(latest_path, "w") as f:
        f.write(content)

    # 尝试通过 hermes send 推送
    short_msg = f"🔴 CI 失败邮件\n{subject}\n\n{body[:500]}"
    try:
        subprocess.run(
            ["hermes", "send", short_msg],
            timeout=10,
            capture_output=True,
            cwd=os.path.expanduser("~")
        )
    except Exception as e:
        print(f"[hermes send 失败: {e}]", file=sys.stderr)


def main():
    os.makedirs(REPORT_DIR, exist_ok=True)

    try:
        mail = imaplib.IMAP4_SSL(IMAP_SERVER, IMAP_PORT)
        mail.login(QQ_EMAIL, QQ_IMAP_CODE)
    except Exception as e:
        print(f"[IMAP 连接失败: {e}]", file=sys.stderr)
        sys.exit(1)

    try:
        mail.select('INBOX')

        # 搜索最近 30 封邮件
        status, messages = mail.search(None, 'ALL')
        if status != 'OK':
            print("[搜索失败]", file=sys.stderr)
            sys.exit(1)

        mail_ids = messages[0].split()
        total = len(mail_ids)
        if total == 0:
            print("[收件箱为空]")
            return

        recent_ids = mail_ids[-30:] if total >= 30 else mail_ids

        state = load_seen_state()
        seen_ids = set(state.get("seen_ids", []))

        new_alerts = []

        for mid in reversed(recent_ids):
            mid_str = mid.decode() if isinstance(mid, bytes) else str(mid)
            if mid_str in seen_ids:
                continue

            status, msg_data = mail.fetch(mid, '(RFC822)')
            if status != 'OK':
                continue

            msg = email.message_from_bytes(msg_data[0][1])

            subject = decode_str(msg['Subject'])
            from_hdr = decode_str(msg['From'])
            date_str = msg.get('Date', '')

            # 筛选 CI 相关邮件
            combined = subject + " " + from_hdr
            if not any(k in combined for k in CI_KEYWORDS):
                seen_ids.add(mid_str)
                continue

            body = extract_body(msg)

            new_alerts.append({
                'mid': mid_str,
                'subject': subject,
                'from': from_hdr,
                'date': date_str,
                'body': body
            })

            seen_ids.add(mid_str)

        # 保存状态
        state["seen_ids"] = list(seen_ids)
        save_seen_state(state)

        if new_alerts:
            print(f"发现 {len(new_alerts)} 封新的 CI 失败邮件")
            for alert in new_alerts:
                print(f"  - {alert['subject']}")
                notify_hermes(alert['subject'], alert['body'], alert['date'])
        else:
            print(f"[{datetime.now().strftime('%H:%M')}] 无新 CI 邮件")

    finally:
        try:
            mail.logout()
        except Exception:
            pass


if __name__ == "__main__":
    main()
