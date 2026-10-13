#!/usr/bin/env python3
"""Send your Claude Code plan limits to Home Assistant, as sensor.claude_code_usage, for the Claude Code usage tile.

Run it on the computer where you use Claude Code (it reads Claude Code's own login), every few minutes:

    python tools/push_usage.py --print           # show what it would send, send nothing
    python tools/push_usage.py                   # send it once
    python tools/push_usage.py --install-task    # Windows: send it every 5 minutes with Task Scheduler
    python tools/push_usage.py --remove-task     # Windows: stop that

Home Assistant's address and a long-lived access token (your profile in Home Assistant, Security) come from the
environment (HA_URL, HA_TOKEN) or from push_usage.ini beside this file:

    [home_assistant]
    url = http://homeassistant.local:8123
    token = eyJ...

The numbers come from the same place as Claude Code's /usage: an address of Anthropic's that is not documented and may
change. When it fails, or Claude Code's login has expired (it renews the next time you use Claude Code), the sensor
keeps its last numbers with `stale: true`. This script never renews the login itself: that would log Claude Code out.
It uses only Python's standard library.
"""
import argparse
import configparser
import datetime as dt
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
INI = HERE / 'push_usage.ini'
STATE = HERE / 'push_usage.state.json'   # the last good numbers, sent again with stale: true when the usage fails
LOG = HERE / 'push_usage.log'
USAGE_URL = 'https://api.anthropic.com/api/oauth/usage'
ENTITY = 'sensor.claude_code_usage'
TASK = 'Tessera Claude Code usage'
TIMEOUT = 15

# The blocks of the usage answer, and the attributes of the sensor they become.
LIMITS = (('five_hour', 'session'), ('seven_day', 'week'))
MODELS = (('seven_day_opus', 'Opus'), ('seven_day_sonnet', 'Sonnet'))


class UsageError(Exception):
    pass


def log(message):
    line = f'{dt.datetime.now():%Y-%m-%d %H:%M:%S} {message}'
    if sys.stderr:
        print(line, file=sys.stderr)
    try:
        # Kept short: the last 200 lines.
        lines = LOG.read_text(encoding='utf-8').splitlines()[-199:] if LOG.exists() else []
        LOG.write_text('\n'.join(lines + [line]) + '\n', encoding='utf-8')
    except OSError:
        pass


def claude_token():
    """Claude Code's login, as it keeps it: ~/.claude/.credentials.json (or CLAUDE_CONFIG_DIR). Never printed."""
    base = Path(os.environ.get('CLAUDE_CONFIG_DIR') or Path.home() / '.claude')
    path = base / '.credentials.json'
    try:
        oauth = json.loads(path.read_text(encoding='utf-8'))['claudeAiOauth']
    except (OSError, ValueError, KeyError, TypeError):
        raise UsageError(f'no Claude Code login in {path}: sign in to Claude Code with your Claude account (/login)')
    token = oauth.get('accessToken')
    if not token:
        raise UsageError('the Claude Code login has no access token')
    expires = oauth.get('expiresAt')  # milliseconds since 1970
    if isinstance(expires, (int, float)) and expires / 1000 < time.time():
        raise UsageError('the Claude Code login has expired; it renews the next time you use Claude Code')
    return token


def epoch(text):
    """An ISO moment of the usage answer as seconds since 1970, or 0."""
    if not text:
        return 0
    try:
        return int(dt.datetime.fromisoformat(str(text).replace('Z', '+00:00')).timestamp())
    except ValueError:
        return 0


def percent(block):
    value = block.get('utilization') if isinstance(block, dict) else None
    return round(float(value), 1) if isinstance(value, (int, float)) else None


def read_usage():
    """The usage answer turned into the sensor's attributes."""
    request = urllib.request.Request(USAGE_URL, headers={
        'Authorization': f'Bearer {claude_token()}',
        'anthropic-beta': 'oauth-2025-04-20',
        'Accept': 'application/json',
        'User-Agent': 'tessera-claude-usage',
    })
    try:
        with urllib.request.urlopen(request, timeout=TIMEOUT) as response:
            answer = json.load(response)
    except urllib.error.HTTPError as e:
        raise UsageError(f'the usage answered {e.code}' + (' (the login has expired?)' if e.code in (401, 403) else ''))
    except (urllib.error.URLError, TimeoutError, ValueError) as e:
        raise UsageError(f'no usage: {e}')
    if not isinstance(answer, dict):
        raise UsageError('the usage answer is not what this script knows')

    attributes = {}
    for key, name in LIMITS:
        block = answer.get(key)
        attributes[f'{name}_pct'] = percent(block)
        attributes[f'{name}_resets_at'] = epoch(block.get('resets_at')) if isinstance(block, dict) else 0
    if attributes['session_pct'] is None:
        raise UsageError('the usage answer has no five_hour limit: is this login a Claude plan (Pro or Max)?')
    if attributes['week_pct'] is None:
        attributes['week_pct'] = 0.0
    # The week of one model, on plans that have one: the first that is there.
    for key, label in MODELS:
        block = answer.get(key)
        if percent(block) is not None:
            attributes.update(model_pct=percent(block), model_name=label, model_resets_at=epoch(block.get('resets_at')))
            break
    return attributes


def settings():
    config = configparser.ConfigParser()
    config.read(INI, encoding='utf-8')
    url = os.environ.get('HA_URL') or config.get('home_assistant', 'url', fallback='')
    token = os.environ.get('HA_TOKEN') or config.get('home_assistant', 'token', fallback='')
    entity = config.get('home_assistant', 'entity', fallback=ENTITY)
    return url.rstrip('/'), token, entity


def push(attributes, url, token, entity):
    body = {
        'state': str(attributes['session_pct']),
        'attributes': {**attributes, 'unit_of_measurement': '%', 'friendly_name': 'Claude Code usage',
                       'icon': 'mdi:robot'},
    }
    request = urllib.request.Request(f'{url}/api/states/{entity}', data=json.dumps(body).encode(), method='POST',
                                     headers={'Authorization': f'Bearer {token}', 'Content-Type': 'application/json'})
    try:
        with urllib.request.urlopen(request, timeout=TIMEOUT) as response:
            response.read()
    except urllib.error.HTTPError as e:
        raise SystemExit(f'Home Assistant answered {e.code}' + (': check the token' if e.code == 401 else ''))
    except (urllib.error.URLError, TimeoutError) as e:
        raise SystemExit(f'Home Assistant cannot be reached at {url}: {e}')


def run(dry_run):
    try:
        attributes = read_usage()
        attributes.update(stale=False, updated_at=int(time.time()))
        STATE.write_text(json.dumps(attributes), encoding='utf-8')
    except UsageError as e:
        log(str(e))
        try:
            attributes = json.loads(STATE.read_text(encoding='utf-8'))
        except (OSError, ValueError):
            raise SystemExit(f'{e}, and there are no earlier numbers to send')
        attributes['stale'] = True
    if dry_run:
        print(json.dumps(attributes, indent=2))
        return
    url, token, entity = settings()
    if not url or not token:
        raise SystemExit(f'Home Assistant is not set: HA_URL and HA_TOKEN, or {INI.name} (see the top of this file)')
    push(attributes, url, token, entity)


def install_task():
    if os.name != 'nt':
        raise SystemExit('--install-task is for Windows; elsewhere run this every 5 minutes with cron or a systemd timer')
    python = Path(sys.executable)
    pythonw = python.with_name('pythonw.exe')
    runner = pythonw if pythonw.exists() else python   # pythonw: no window every 5 minutes
    command = f'"{runner}" "{Path(__file__).resolve()}"'
    subprocess.run(['schtasks', '/Create', '/F', '/SC', 'MINUTE', '/MO', '5', '/TN', TASK, '/TR', command], check=True)
    print(f'Task Scheduler runs this every 5 minutes as "{TASK}". Problems go to {LOG}.')


def remove_task():
    subprocess.run(['schtasks', '/Delete', '/F', '/TN', TASK], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--print', action='store_true', help='show what would be sent, send nothing')
    parser.add_argument('--install-task', action='store_true', help='Windows: run this every 5 minutes')
    parser.add_argument('--remove-task', action='store_true', help='Windows: stop running it')
    args = parser.parse_args()
    if args.install_task:
        install_task()
    elif args.remove_task:
        remove_task()
    else:
        try:
            run(args.print)
        except SystemExit as e:
            if isinstance(e.code, str) and not sys.stderr:   # pythonw has no console: the log is where it shows
                log(e.code)
            raise


if __name__ == '__main__':
    main()
