import json, urllib.request, time, sys, os

token = os.environ.get('GITHUB_TOKEN', '')
repo = 'Fandel-Chuang/chaos-engine'
branch = 'fix/loopengine-github-ci-dbproxy'

for attempt in range(40):
    url = f'https://api.github.com/repos/{repo}/actions/runs?per_page=5'
    req = urllib.request.Request(url, headers={'Accept': 'application/vnd.github+json', 'Authorization': f'Bearer {token}'})
    with urllib.request.urlopen(req, timeout=15) as resp:
        data = json.loads(resp.read())
    found = False
    for r in data.get('workflow_runs', []):
        if r.get('head_branch') == branch:
            found = True
            run_id = r['id']
            status = r.get('status') or '?'
            conclusion = r.get('conclusion') or '?'
            print(f'[{attempt}] {status:12} | {conclusion:10}')
            sys.stdout.flush()
            if status == 'completed':
                jobs_url = f'https://api.github.com/repos/{repo}/actions/runs/{run_id}/jobs'
                req2 = urllib.request.Request(jobs_url, headers={'Accept': 'application/vnd.github+json', 'Authorization': f'Bearer {token}'})
                with urllib.request.urlopen(req2, timeout=10) as resp2:
                    jobs_data = json.loads(resp2.read())
                all_ok = True
                for job in jobs_data.get('jobs', []):
                    jc = job.get('conclusion') or '?'
                    jn = job.get('name', '?')
                    if jc in ('failure', 'cancelled'):
                        print(f'  X {jn}')
                        all_ok = False
                    elif jc == 'success':
                        print(f'  OK {jn}')
                sys.stdout.flush()
                if all_ok:
                    print('=== CI ALL PASSED ===')
                    merge_url = f'https://api.github.com/repos/{repo}/pulls/9/merge'
                    merge_data = json.dumps({
                        'commit_title': 'fix: LoopEngine GitHub CI 支持 + dbproxy table_count 修复 (#9)',
                        'merge_method': 'squash'
                    }).encode()
                    merge_req = urllib.request.Request(merge_url, data=merge_data, method='PUT', headers={
                        'Accept': 'application/vnd.github+json',
                        'Authorization': f'Bearer {token}',
                        'Content-Type': 'application/json'
                    })
                    try:
                        with urllib.request.urlopen(merge_req, timeout=15) as merge_resp:
                            merge_result = json.loads(merge_resp.read())
                            print(f'=== MERGED: {merge_result.get("message", "?")} ===')
                            print(f'SHA: {merge_result.get("sha", "?")}')
                    except Exception as e:
                        print(f'Merge failed: {e}')
                sys.exit(0 if all_ok else 1)
            break
    if not found:
        print(f'[{attempt}] waiting...')
        sys.stdout.flush()
    time.sleep(15)
