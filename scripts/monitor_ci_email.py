#!/usr/bin/env python3
"""
QQ 邮箱 CI 失败邮件监控脚本
- 定时调用，读取 QQ 邮箱中 CI 失败通知邮件
- 只输出新发现的 CI 失败邮件（去重）
- 无新邮件时输出为空（静默，不推送）

用法: QQ_IMAP_CODE=xxxx python3 monitor_ci_email.py
环境变量:
  QQ_EMAIL       QQ 邮箱地址（默认 zfdtc1111@qq.com）
  QQ_IMAP_CODE   IMAP 授权码（必须提供）
"""
import imaplib
import email
import os
import sys
import json
import re
from email.header import decode_header
from datetime import datetime

# ============ 配置 ============
QQ_EMAIL = os.environ.get("QQ_EMAIL", "zfdtc1111@qq.com")
QQ_IMAP_CODE = os.environ.get("QQ_IMAP_CODE", "")
IMAP_SERVER = "imap.qq.com"
IMAP_PORT = 993

if not QQ_IMAP_CODE:
    print("ERROR: 请设置环境变量 QQ_IMAP_CODE", file=sys.stderr)
    sys.exit(1)

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPORT_DIR = os.path.join(PROJECT_DIR, ".ci-reports", "qq-mail-alerts")
STATE_FILE = os.path.join(REPORT_DIR, "notified_ids.json")

# CI 失败邮件关键词（精确匹配主题和发件人）
CI_FAIL_KEYWORDS = ['failed', 'failure', '失败', 'build failed',
                    'CI failed', 'run failed', 'workflow run']


def decode_str(raw):
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
    body = re.sub(r'<[^>]+>', ' ', body)
    body = re.sub(r'\s+', ' ', body).strip()
    return body[:2000]


def load_notified():
    """加载已推送过的 CI 失败邮件 ID"""
    if os.path.exists(STATE_FILE):
        try:
            with open(STATE_FILE) as f:
                data = json.load(f)
                return set(data.get("notified_ids", []))
        except Exception:
            pass
    return set()


def save_notified(notified):
    os.makedirs(REPORT_DIR, exist_ok=True)
    ids = list(notified)
    if len(ids) > 200:
        ids = ids[-200:]
    with open(STATE_FILE, "w") as f:
        json.dump({"notified_ids": ids}, f, indent=2, ensure_ascii=False)


def is_ci_fail_email(subject, from_hdr):
    """判断是否为 CI 失败邮件"""
    combined = (subject + " " + from_hdr).lower()
    return any(k.lower() in combined for k in CI_FAIL_KEYWORDS)


def main():
    os.makedirs(REPORT_DIR, exist_ok=True)

    try:
        mail = imaplib.IMAP4_SSL(IMAP_SERVER, IMAP_PORT)
        mail.login(QQ_EMAIL, QQ_IMAP_CODE)
    except Exception as e:
        print(f"IMAP 连接失败: {e}", file=sys.stderr)
        sys.exit(1)

    try:
        mail.select('INBOX')
        status, messages = mail.search(None, 'ALL')
        if status != 'OK':
            sys.exit(1)

        mail_ids = messages[0].split()
        if not mail_ids:
            return

        # 检查最近 30 封
        recent_ids = mail_ids[-30:]

        notified = load_notified()
        new_alerts = []

        for mid in reversed(recent_ids):
            mid_str = mid.decode() if isinstance(mid, bytes) else str(mid)

            # 先 fetch 信头，判断是否 CI 失败邮件
            status, msg_data = mail.fetch(mid, '(BODY[HEADER.FIELDS (SUBJECT FROM DATE)])')
            if status != 'OK':
                continue

            msg = email.message_from_bytes(msg_data[0][1])
            subject = decode_str(msg['Subject'])
            from_hdr = decode_str(msg['From'])

            # 不是 CI 失败邮件就跳过（不记录 ID，不污染状态）
            if not is_ci_fail_email(subject, from_hdr):
                continue

            # 是 CI 失败邮件，但已经推送过了
            if mid_str in notified:
                continue

            # 新的 CI 失败邮件！fetch 完整内容
            status, full_data = mail.fetch(mid, '(RFC822)')
            if status != 'OK':
                continue
            full_msg = email.message_from_bytes(full_data[0][1])
            body = extract_body(full_msg)
            date_str = full_msg.get('Date', '')

            new_alerts.append({
                'mid': mid_str,
                'subject': subject,
                'from': from_hdr,
                'date': date_str,
                'body': body
            })
            notified.add(mid_str)

        save_notified(notified)

        # 只有新 CI 失败邮件才输出（推送），否则静默
        if new_alerts:
            lines = []
            for a in new_alerts:
                lines.append(f"🔴 CI 失败邮件")
                lines.append(f"主题: {a['subject']}")
                lines.append(f"时间: {a['date']}")
                lines.append(f"内容: {a['body'][:500]}")
                lines.append("")

                # 存档
                ts = datetime.now().strftime("%Y%m%d_%H%M%S")
                with open(os.path.join(REPORT_DIR, f"alert-{ts}.md"), "w") as f:
                    f.write(f"# CI 失败告警\n\n- 主题: {a['subject']}\n- 时间: {a['date']}\n\n{a['body']}")

            print("\n".join(lines))

    finally:
        try:
            mail.logout()
        except Exception:
            pass


if __name__ == "__main__":
    main()
