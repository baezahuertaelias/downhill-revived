#!/usr/bin/env python3
"""One unattended test of the Switch build on a console: every phase launches the game by itself,
and everything lands in build/nx-test/<timestamp>/ (report.md, raw logs, PNG captures).

    python3 port/tools/nx_fulltest.py --ip CONSOLE_IP
    python3 port/tools/nx_fulltest.py --only BC        (the console's IP may come from DHD_SWITCH_IP)
    python3 port/tools/nx_fulltest.py --dry-run        (no network: the settings of each phase, the checks of local files)

The console runs the sphaira menu with its FTP server (port 5000, anonymous, root is sdmc:) and its
nxlink receiver. Standard library only, plus curl for FTP (its resume is what works on this server
and this Wi-Fi), macOS's sips for the PNGs (without it the BMPs stay) and devkitPro's nxlink.

Phases: A preflight, B title and menus, C race benchmark, D memory card, E state trace, F post.
A failed preflight stops the run; any other failed phase does not stop the next. The player's
dhd.ini is saved first and comes back at the end, re-read to check it landed, also on errors and
Ctrl+C. A dhd.ini left behind by a killed run is recognised by its first line, and the newest saved
copy under build/nx-test/ is then the original.

How a launch ends. nxlink -s stays open as long as the game's stream does, and over this Wi-Fi it
can outlive the game or die before it. So the FTP port is the witness: nothing answers there while
the game runs, and sphaira answers again when it has ended. DHD_NX_EXIT is always set as a watchdog,
a little under the phase's timeout, so that a stuck race still gives the console back.

Settings. The game's environment comes only from dhd.ini, and the file must exist, so DHD_SETTINGS=none
(the recipe of state_trace_run.py) can't work as it does on a PC: with a file present, settings_nx.c
gives every key in its table the player's default. What the recipe gets from "no file" is therefore
written as lines: widescreen off (an "off" that is the same as unset) and the 4:3 picture. The other
defaults of the table equal the code's behaviour when unset (fps 60, scale 2, font sharp, vibration
on, textures on). DHD_SETTINGS = none is still kept as a line: dhd.ini has been read by the time it
takes effect, and from then on game_options neither reads nor writes the file, as in the recipe.
The first line of a key wins and a value is at most 1023 bytes (settings_nx.c), which this tool checks.

Recipes are read from bench_race.py and state_trace_run.py, not copied: only the needed top-level
statements of those files run (their imports want numpy, PIL and a Windows job object).

Under nxlink the game's stderr goes to the stream and dhd.log is not written (nx_host.c); a launch
by hand (the fallback when nxlink keeps failing) writes dhd.log and no stream."""
import argparse
import ast
import datetime
import decimal
import hashlib
import os
import re
import select
import shutil
import signal
import socket
import statistics
import subprocess
import sys
import tempfile
import time
import traceback
import types
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'port/tools'
GAME_DIR = '/switch/downhill-revived'
SD_GAME = 'sdmc:' + GAME_DIR
TEST_DIR = GAME_DIR + '/nxtest'       # everything this tool leaves on the card goes here; removed at the end
SD_TEST = 'sdmc:' + TEST_DIR
INI = GAME_DIR + '/dhd.ini'
CRASH_DIR = '/atmosphere/crash_reports'
SYS_CLK = '/config/sys-clk/config.ini'
ISO_NAME, ISO_SIZE = 'SCUS-97177.iso', 2530869248
MARKER = b'; nx_fulltest test settings'
VALUE_MAX = 1023                      # settings_nx.c: char v[1024]
TARGET_FPS = 60
# The player's fresh dhd.ini (nx_host.c writes these on a first start).
FRESH = {'iso': SD_GAME + '/' + ISO_NAME, 'DHD_SCALE': '1', 'DHD_WINDOW': 'fullscreen', 'DHD_SKIP_MOVIES': '1',
         'auto_update': 'off'}
# "No dhd.ini" as lines; see the top of this file.
NO_FILE = {'iso': SD_GAME + '/' + ISO_NAME, 'DHD_WIDESCREEN': '0', 'DHD_ASPECT': '4:3', 'auto_update': 'off'}
BLANK_ALIASES = {'DHD_ISO': 'iso'}    # a key the table knows under two names: one line only
TRACE_SKIP = {'combined', 'stackwords', 'field'}  # as trace_columns.py
# Env of the trace header that may differ: paths, and DHD_FPS, which the table sets to 60 (the code's value when unset)
HEADER_IGNORED = {'DHD_STATE_MASK', 'DHD_STATE_CODEPTRS', 'DHD_FPS'}
CUMULATIVE = {'t', 'field', 'fields', 'picture', 'pictures', 'present', 'presents', 'shown', 'draw', 'draws', 'command',
              'commands', 'presented'}
ERROR = re.compile(r"error|fail|fatal|abort|panic|assert|exception|out of memory|invalid|cannot|could not|no room|"
                   r"didn't end|freeze:|\btrap|fault", re.I)
NOT_ERROR = re.compile(r'gpu error 0\b|^bench|fault guard', re.I)
HARD_ERROR = re.compile(r'gpu error [1-9-]|fatal|panic|host64 memory:|out of memory|abort', re.I)
THERMAL = re.compile(r'therm|temperature|\btemp\b|\bsoc\b.*\bC\b|\bfan\b', re.I)


def say(text):
    print(text, flush=True)


# ---- the recipes, read from the tools that own them ----

def slice_module(path, names, namespace):
    """Run only the top-level assignments and functions called `names` of a Python file, in order."""
    tree = ast.parse(Path(path).read_text(encoding='utf-8'))
    found = set()
    for node in tree.body:
        if isinstance(node, ast.Assign):
            targets = {t.id for t in node.targets if isinstance(t, ast.Name)}
        elif isinstance(node, ast.FunctionDef):
            targets = {node.name}
        else:
            continue
        if targets & set(names):
            found |= targets
            exec(compile(ast.Module([node], []), str(path), 'exec'), namespace)
    missing = set(names) - found
    if missing:
        raise SystemExit('%s no longer defines %s' % (path, ', '.join(sorted(missing))))
    return namespace


def read_recipes():
    compare = slice_module(TOOLS / 'compare_pcsx2.py', ['STANDARD_PAD', 'RATE', 'BUTTONS', 'field_at', 'parse_pad', 'frame_script'],
                           {'Decimal': decimal.Decimal, 'ROUND_CEILING': decimal.ROUND_CEILING})
    compare = types.SimpleNamespace(**compare)
    race = types.SimpleNamespace(**slice_module(TOOLS / 'race_compare.py', ['NAVIGATION', 'NAV', 'RACE_PAD'], {'compare': compare}))
    scenes = slice_module(TOOLS / 'bench_race.py', ['SCENES'], {'compare': compare, 'race': race})['SCENES']
    trace = slice_module(TOOLS / 'state_trace_run.py',
                         ['BOOT_NAV', 'RACE_NAV', 'RACE_PAD', 'DEFAULT_FIELDS', 'DEFAULT_SEED', 'UNLOCKED_TRACKS', 'FULL_PEDAL',
                          'SPLIT_NAV', 'OPTIONS_NAV', 'track_nav', 'SCENARIOS', 'build_env'], {'os': os, 'Path': Path})
    return scenes, trace


def recipe_env(trace, scenario, prefix, fields, sha):
    """state_trace_run.py's own build_env, without the host's environment; paths are the card's."""
    args = types.SimpleNamespace(iso=SD_GAME + '/' + ISO_NAME, scenario=scenario, race_pad=None, seed=trace['DEFAULT_SEED'],
                                 mask=None, codeptrs=None, dump=None, env=[])
    env = trace['build_env'](args, prefix, sha, fields)
    return {k: v for k, v in env.items() if k.startswith('DHD_')}


def ini_text(phase, base, env):
    """A phase's dhd.ini: the marker, the phase's name (seen in the log's settings line), the lines."""
    lines = dict(base)
    for key, value in env.items():
        lines[BLANK_ALIASES.get(key, key)] = value
    out = ['%s, phase %s; the player\'s own file comes back after the run' % (MARKER.decode(), phase), 'DHD_NXTEST_PHASE = ' + phase]
    for key, value in lines.items():
        if len(value) > VALUE_MAX:
            raise SystemExit('dhd.ini value of %s is %d bytes, the game reads %d' % (key, len(value), VALUE_MAX))
        out.append('%s = %s' % (key, value))
    return ('\n'.join(out) + '\n').encode()


# ---- FTP, by curl ----

class FtpError(Exception):
    pass


class Ftp:
    MISSING = (9, 19, 78)  # curl: can't enter the directory, can't retrieve, no such file

    def __init__(self, ip, port, log_path):
        self.ip, self.port, self.log_path = ip, port, log_path
        self.curl = shutil.which('curl')
        if not self.curl:
            raise SystemExit('no curl on PATH')

    def url(self, path):
        return 'ftp://%s:%d%s' % (self.ip, self.port, path)

    def run(self, args, timeout=120):
        cmd = [self.curl, '-sS', '--connect-timeout', '10', '--speed-limit', '1024', '--speed-time', '30', *args]
        try:
            p = subprocess.run(cmd, capture_output=True, timeout=timeout)
            rc, out, err = p.returncode, p.stdout, p.stderr
        except subprocess.TimeoutExpired:
            rc, out, err = 28, b'', b'timed out here'
        with open(self.log_path, 'a', encoding='utf-8') as log:
            log.write('%s rc=%d %s %s\n' % (time.strftime('%H:%M:%S'), rc, ' '.join(args), err.decode('utf-8', 'replace').strip()))
        return rc, out

    def alive(self, timeout=3.0):
        """Does sphaira's FTP server answer? (While the game runs nothing listens there.)"""
        try:
            with socket.create_connection((self.ip, self.port), timeout=timeout) as s:
                s.settimeout(timeout)
                return s.recv(64)[:1] in (b'2', b'1')
        except OSError:
            return False

    def wait_alive(self, seconds):
        end = time.monotonic() + seconds
        while True:
            if self.alive():
                return True
            if time.monotonic() > end:
                return False
            time.sleep(3)

    def _retry(self, what, attempt, attempts=5):
        for k in range(attempts):
            result = attempt()
            if result is not None:
                return result
            if k + 1 < attempts:
                time.sleep(2 + 2 * k)
        raise FtpError(what)

    def list(self, folder):
        """A folder's entries as (kind, size, name), or None when it isn't there."""
        def attempt():
            rc, out = self.run([self.url(folder.rstrip('/') + '/')])
            if rc in self.MISSING:
                return 'missing'
            return None if rc else out
        out = self._retry('listing %s' % folder, attempt)
        if out == 'missing':
            return None
        return parse_list(out.decode('utf-8', 'replace'))

    def size(self, path):
        folder, _, name = path.rpartition('/')
        for kind, size, entry in self.list(folder) or []:
            if entry == name and kind != 'd':
                return size
        return None

    def get(self, path, local, attempts=8):
        """Download with resume. False when the file isn't there; FtpError when it can't be had."""
        local = Path(local)
        local.parent.mkdir(parents=True, exist_ok=True)
        if local.exists():
            local.unlink()

        def attempt():
            resume = ['-C', '-'] if local.exists() and local.stat().st_size else []
            rc, _ = self.run([*resume, '-o', str(local), self.url(path)], timeout=1800)
            if rc in self.MISSING:
                return 'missing'
            return None if rc else 'ok'
        return self._retry('download of %s' % path, attempt, attempts) == 'ok'

    def read(self, path):
        with tempfile.TemporaryDirectory() as t:
            local = Path(t) / 'f'
            if not self.get(path, local):
                return None
            return local.read_bytes() if local.exists() else b''  # curl may make no file for an empty one

    def put(self, data, path):
        """A small file, whole, and read back."""
        with tempfile.TemporaryDirectory() as t:
            local = Path(t) / 'f'
            local.write_bytes(data)

            def attempt():
                rc, _ = self.run(['--ftp-create-dirs', '-T', str(local), self.url(path)])
                return 'ok' if not rc and self.read(path) == data else None
            self._retry('upload of %s' % path, attempt)

    def upload_resume(self, local, path, attempts=25):
        """A big file: over the Wi-Fi's broken pipes, each try continues the last (curl -C -). The
        first try overwrites, because a file of the same size from another build would look finished."""
        size = Path(local).stat().st_size
        for k in range(attempts):
            rc, _ = self.run(['--ftp-create-dirs', *(['-C', '-'] if k else []), '-T', str(local), self.url(path)], timeout=1800)
            try:
                now = self.size(path)
            except FtpError:
                now = None
            say('  upload try %d: curl %d, %s of %d bytes on the card' % (k + 1, rc, now, size))
            if now == size:
                return k + 1
            if now is not None and now > size:
                raise FtpError('%s is larger on the card than here' % path)
            self.wait_alive(60)
            time.sleep(3)
        raise FtpError('upload of %s never finished' % path)

    def same_range(self, local, path, start, length):
        data = Path(local).read_bytes()[start:start + length]
        rc, out = self.run(['-r', '%d-%d' % (start, start + length - 1), self.url(path)])
        return out == data, rc

    def delete(self, path, rmdir=False):
        """Removes only what this tool puts there: the test folder, captures, a dhd.ini it made, the log."""
        allowed = path == INI or path == GAME_DIR + '/dhd.log' or path == TEST_DIR or path.startswith(TEST_DIR + '/') or \
            re.fullmatch(re.escape(GAME_DIR) + r'/cap_[^/]*\.bmp', path)
        if not allowed:
            raise FtpError('refusing to delete %s' % path)
        rc, _ = self.run(['-o', os.devnull, '-Q', '%s %s' % ('RMD' if rmdir else 'DELE', path), self.url('/')])
        return rc == 0

    def walk(self, folder):
        """Every file below a folder: (path, size), and its folders, deepest last."""
        files, folders = [], [folder]
        for kind, size, name in self.list(folder) or []:
            if name in ('.', '..'):
                continue
            if kind == 'd':
                inner, deeper = self.walk(folder + '/' + name)
                files += inner
                folders += deeper
            else:
                files.append((folder + '/' + name, size))
        return files, folders

    def remove_tree(self, folder):
        files, folders = self.walk(folder)
        for path, _ in files:
            self.delete(path)
        for path in reversed(folders):
            self.delete(path, rmdir=True)


def parse_list(text):
    """LIST lines, leniently: Unix style, "... SIZE Mon DD HH:MM|YYYY NAME"."""
    entries = []
    for line in text.splitlines():
        m = re.match(r'([-dl])\S*\s+(?:\S+\s+)*?(\d+)\s+[A-Za-z]{3}\s+\d+\s+[\d:]+\s+(.*\S)\s*$', line)
        if m:
            entries.append((m.group(1), int(m.group(2)), m.group(3)))
    return entries


# ---- reading a run's log ----

def number_pairs(line):
    return {k: float(v) for k, v in re.findall(r'\b([A-Za-z_][\w.]*)[=:]\s*(-?\d+(?:\.\d+)?)', line)}


def heartbeat_values(line):
    """A once-a-second line's numbers: "key=1.5" and the older "field 12 pictures 3"."""
    values = number_pairs(line)
    for k, v in re.findall(r'\b([A-Za-z_]+) (-?\d+(?:\.\d+)?)\b', line):
        values.setdefault(k, float(v))
    return values


def parse_log(entries):
    """entries: (arrival seconds or None, text). What the report needs."""
    a = dict(bench=[], bench_main=None, threads=None, interp=None, snd=None, errors={}, hard=[], settings=None, nav=None,
             go=None, thermal=[], rows=[], other={}, last_field=None)
    shapes = {}
    for arrival, line in entries:
        s = line.strip()
        if not s:
            continue
        if s.startswith('bench'):
            a['bench'].append(s)
            if s.startswith('bench:'):
                a['bench_main'] = number_pairs(s)
                m = re.search(r'fields=(\d+)-(\d+)', s)
                a['bench_main']['window'] = m.group(0)[7:] if m else ''
            elif s.startswith('bench threads:'):
                a['threads'] = number_pairs(s)
            elif s.startswith('bench interp'):
                a['interp'] = s
            elif s.startswith('bench snd'):
                a['snd'] = s
        if s.startswith('settings:'):
            a['settings'] = s
        m = re.search(r'compare: nav step (\d+)', s)
        if m:
            a['nav'] = int(m.group(1))
        m = re.search(r'race anchor=g field=(\d+)', s)
        if m:
            a['go'] = int(m.group(1))
        if ERROR.search(s) and not NOT_ERROR.search(s):
            key = re.sub(r'0x[0-9a-fA-F]+|\d+', '#', s)[:160]
            e = a['errors'].setdefault(key, dict(count=0, first=s[:220]))
            e['count'] += 1
        if HARD_ERROR.search(s) and not NOT_ERROR.search(s):
            a['hard'].append(s[:220])
        if THERMAL.search(s) and not s.startswith(('settings:', 'compare:')):
            a['thermal'].append(s[:220])
        if not s.startswith(('compare:', 'nx capture', 'nx present', 'bench')) and re.match(r'(nx\b|heartbeat)', s):
            values = heartbeat_values(s)
            if 'field' in values or 'fields' in values:
                sig = frozenset(values)
                shapes.setdefault(sig, []).append((arrival, values))
    # The shape with the most lines that counts fields gives the speed; every shape's other numbers are timers.
    if shapes:
        main = max(shapes.values(), key=len)
        a['rows'] = speed_rows(main)
        a['last_field'] = main[-1][1].get('field', main[-1][1].get('fields'))
        collected = {}
        for rows in shapes.values():
            for _, values in rows:
                for k, v in values.items():
                    if k.lower() not in CUMULATIVE:
                        collected.setdefault(k, []).append(v)
        a['other'] = {k: statistics.median(v) for k, v in collected.items() if len(v) >= 2}
    return a


def speed_rows(rows):
    """(time, field, fields/s since the row before, pictures, draws), from the once-a-second lines.
    A "field(s)" that isn't cumulative is taken as already being per second."""
    key = 'field' if 'field' in rows[0][1] else 'fields'
    fields = [v[key] for _, v in rows]
    rising = all(y >= x for x, y in zip(fields, fields[1:]))
    out, previous = [], None
    for arrival, v in rows:
        t = v.get('t', arrival)
        rate = None
        if not rising:
            rate = v[key]
        elif previous is not None and t is not None and previous[0] is not None and t > previous[0]:
            rate = (v[key] - previous[1]) / (t - previous[0])
        out.append(dict(t=t, field=v[key], rate=rate, pictures=v.get('pictures'), draws=v.get('draws')))
        previous = (t, v[key])
    return out


def race_summary(a, go_default=3650):
    """The heartbeat's speed after GO, and the heaviest stretch (the slowest five lines in a row)."""
    go = a['go'] or go_default
    rows = [r for r in a['rows'] if r['field'] >= go]
    timed = [r for r in rows if r['rate'] is not None]
    if not timed:
        return None
    result = dict(go=go, median=statistics.median(r['rate'] for r in timed), low=min(r['rate'] for r in timed), lines=len(timed))
    per = draws_per_picture(rows)
    if per is not None:
        result['draws_per_picture'] = per
    width = 5
    if len(rows) > width:
        worst = None
        for k in range(len(rows) - width):
            first, last = rows[k], rows[k + width]
            if first['t'] is None or last['t'] is None or last['t'] <= first['t']:
                continue
            rate = (last['field'] - first['field']) / (last['t'] - first['t'])
            if worst is None or rate < worst[0]:
                worst = (rate, first, last)
        if worst:
            result['heavy'] = dict(rate=worst[0], first=int(worst[1]['field']), last=int(worst[2]['field']),
                                   draws_per_picture=draws_per_picture([worst[1], worst[2]]))
    return result


def draws_per_picture(rows):
    usable = [r for r in rows if r['pictures'] is not None and r['draws'] is not None]
    if len(usable) < 2 or usable[-1]['pictures'] <= usable[0]['pictures']:
        return None
    return (usable[-1]['draws'] - usable[0]['draws']) / (usable[-1]['pictures'] - usable[0]['pictures'])


# ---- traces ----

def load_trace(path):
    env, columns, rows = {}, None, {}
    for line in open(path, encoding='utf-8', errors='replace'):
        line = line.rstrip('\n')
        if line.startswith('#'):
            m = re.search(r'\benv=(.*)$', line)
            if m:
                env = dict(item.split('=', 1) for item in m.group(1).split() if '=' in item)
        elif line.startswith('field\t'):
            columns = line.split('\t')
        elif line[:1].isdigit() and columns:
            cells = line.split('\t')
            if len(cells) == len(columns):
                rows[int(cells[0])] = dict(zip(columns, cells))
    return env, columns or [], rows


def compare_traces(mine, reference, probe_field):
    """Per column, the first field that differs, by field number. Up to the probes and overall."""
    env_a, columns_a, rows_a = load_trace(mine)
    env_b, columns_b, rows_b = load_trace(reference)
    columns = [c for c in columns_b if c in columns_a and c not in TRACE_SKIP]
    first = {}
    common = sorted(set(rows_a) & set(rows_b))
    for field in common:
        x, y = rows_a[field], rows_b[field]
        for c in columns:
            if c not in first and x[c] != y[c]:
                first[c] = field
    before = [f for f in common if f < probe_field]
    expected_before = [f for f in rows_b if f < probe_field]
    early = sorted((f, c) for c, f in first.items() if f < probe_field)
    env_diff = {k: (env_a.get(k), env_b.get(k)) for k in set(env_a) | set(env_b)
                if env_a.get(k) != env_b.get(k) and k not in HEADER_IGNORED}
    return dict(columns=columns, first=first, rows=len(rows_a), reference_rows=len(rows_b),
                full_before=len(before) == len(expected_before) and len(before) > 0, early=early,
                whole=not first and len(rows_a) == len(rows_b), env_diff=env_diff)


# ---- the run ----

class Phase:
    def __init__(self, key, title):
        self.key, self.title = key, title
        self.status, self.reason = 'NOT RUN', 'not selected (--only)'
        self.details = []
        self.analysis = None
        self.crashes = []
        self.captures = []
        self.wall = None

    def done(self, status, reason):
        self.status, self.reason = status, reason


class GameRun:
    def __init__(self, mode, launched, ended, log, entries, notes):
        self.mode, self.launched, self.ended, self.log, self.entries, self.notes = mode, launched, ended, log, entries, notes
        self.seconds = None


class ConsoleGone(Exception):
    pass


class Test:
    def __init__(self, args):
        self.args = args
        stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
        self.out = Path(args.out) if args.out else ROOT / 'build/nx-test' / stamp
        self.out.mkdir(parents=True, exist_ok=True)
        self.phases = {k: Phase(k, t) for k, t in (('A', 'Preflight'), ('B', 'Title and menus'), ('C', 'Race benchmark'),
                                                   ('D', 'Memory card: save, load'), ('E', 'State trace'), ('F', 'Post'))}
        self.scenes, self.trace = read_recipes()
        self.ftp = None if args.dry_run else Ftp(args.ip, args.ftp_port, self.out / 'curl.log')
        self.nro = Path(args.nro)
        self.nro_sha = None
        self.original = None          # bytes of the player's dhd.ini; None when there was none
        self.backup_taken = False
        self.ini_touched = False
        self.restore_note = 'not changed'
        self.crash_seen = set()
        self.console_gone = False
        self.launches = 0
        self.notes = []

    # ---- settings of each phase ----

    def env_b(self):
        env, window = self.scenes['menu']
        return FRESH, {**env, 'DHD_BENCH': window, 'DHD_MEMCARD': '0', 'DHD_NX_EXIT': str(self.args.menu_seconds), 'DHD_NX_CAPTURE': '1',
                       'DHD_NX_HEARTBEAT': '1'}

    def env_c(self):
        env, window = self.scenes['race']
        settings = {**env, 'DHD_MEMCARD': '0', 'DHD_BENCH': window, 'DHD_BENCH_EXIT': '1', 'DHD_NX_CAPTURE': '1', 'DHD_NX_HEARTBEAT': '1',
                    'DHD_NX_EXIT': str(self.args.race_timeout - 30)}
        if self.args.no_capture:
            del settings['DHD_NX_CAPTURE']
        return FRESH, {**settings, **dict(kv.split('=', 1) for kv in self.args.set)}

    def env_d(self):
        fields = self.trace['SCENARIOS']['memcard'][2]
        env = recipe_env(self.trace, 'memcard', SD_TEST + '/memcard', fields, self.nro_sha or '-')
        for k in ('DHD_STATE_TRACE', 'DHD_STATE_STOP', 'DHD_STATE_EXE'):
            env.pop(k, None)
        env['DHD_STOP_FIELD'] = str(fields)  # the recipe's length, with nothing traced (state_trace.c)
        env['DHD_NX_EXIT'] = str(self.args.save_timeout - 30)
        return NO_FILE, env

    def env_e(self):
        fields = self.args.trace_fields
        env = recipe_env(self.trace, 'race', SD_TEST + '/trace', fields, self.nro_sha or '-')
        env.update(DHD_STATE_MASK=SD_TEST + '/state_trace_mask.txt', DHD_STATE_CODEPTRS=SD_TEST + '/state_trace_codeptrs.txt',
                   DHD_NX_EXIT=str(self.args.trace_timeout - 30))
        return NO_FILE, env

    PHASE_ENV = {'B': 'env_b', 'C': 'env_c', 'D': 'env_d', 'E': 'env_e'}

    def ini_for(self, key):
        base, env = getattr(self, self.PHASE_ENV[key])()
        return ini_text(key, base, env)

    # ---- launching ----

    def nxlink_once(self, log_path, timeout):
        """One send and the stream after it. launched is False when the game never started."""
        import pty
        master, slave = pty.openpty()  # nxlink block-buffers into a pipe
        cmd = [str(self.args.nxlink), '-a', self.args.ip, '-s', str(self.nro)]
        proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=slave, stderr=slave, start_new_session=True)
        os.close(slave)
        entries, partial, start = [], b'', time.monotonic()
        active, seen_game, downs, ups, next_probe = None, False, 0, 0, 0.0
        launched, ended, notes = True, None, []
        eof = False
        with open(log_path, 'wb') as log:
            try:
                while True:
                    ready, _, _ = select.select([master], [], [], 1.0)
                    now = time.monotonic()
                    if ready:
                        try:
                            data = os.read(master, 65536)
                        except OSError:
                            data = b''
                        if data:
                            log.write(data)
                            log.flush()
                            partial += data
                            *whole, partial = partial.split(b'\n')
                            for raw in whole:
                                text = raw.decode('utf-8', 'replace').rstrip('\r')
                                entries.append((now, text))
                                if self.args.verbose:
                                    say('    ' + text)
                                if active is None and 'server active' in text.lower():
                                    active = now
                                    say('  nxlink: sent, server active')
                                elif active is not None and re.match(r'(nx:|gs:|bench|settings:|Downhill)', text):
                                    seen_game = True
                            if active is None and b'server active' in partial.lower():
                                active = now
                                say('  nxlink: sent, server active')
                        else:
                            eof = True
                    if not ready and proc.poll() is not None:
                        eof = True
                    if active is None:
                        if eof:
                            return GameRun('nxlink', False, None, log_path, entries, ['nxlink ended before "server active"'])
                        if now - start > self.args.send_timeout:
                            return GameRun('nxlink', False, None, log_path, entries,
                                           ['no "server active" in %d s' % self.args.send_timeout])
                        continue
                    if eof:
                        break
                    if now - active > timeout:
                        ended = 'timeout'
                        break
                    if now >= next_probe:
                        next_probe = now + 10
                        if self.ftp.alive(2):
                            ups += 1
                        else:
                            downs += 1
                            ups = 0
                        if downs and ups >= 2:  # gone, then back: a run this short that nxlink outlives waits for the timeout
                            ended = 'exit'
                            notes.append('nxlink still open when sphaira came back; stopped it')
                            drain = time.monotonic() + 5
                            while time.monotonic() < drain:
                                ready, _, _ = select.select([master], [], [], 1.0)
                                if ready:
                                    try:
                                        data = os.read(master, 65536)
                                    except OSError:
                                        break
                                    if not data:
                                        break
                                    log.write(data)
                                    partial += data
                            break
                        if not seen_game and not downs and now - active > 120:
                            return GameRun('nxlink', False, None, log_path, entries,
                                           ['"server active", but sphaira still answers after 120 s'])
            finally:
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except OSError:
                    pass
                proc.wait()
                os.close(master)
        for raw in partial.split(b'\n'):
            if raw:
                entries.append((time.monotonic(), raw.decode('utf-8', 'replace').rstrip('\r')))
        if ended is None:  # the stream closed: the game has ended, or the Wi-Fi took the stream
            left = max(30, timeout - (time.monotonic() - active))
            if not seen_game and not downs and self.ftp.alive():
                return GameRun('nxlink', False, None, log_path, entries, ['nxlink ended and the game never showed'])
            ended = 'exit' if self.ftp.wait_alive(left) else 'timeout'
            if not seen_game:
                notes.append('stream closed with no game output')
        return GameRun('nxlink', launched, ended, log_path, entries, notes)

    def manual_launch(self, log_path, timeout):
        """nxlink keeps failing: the NRO on the card, started by a person; the log from the card."""
        wait = self.args.manual_wait
        if wait <= 0:
            return GameRun('manual', False, None, log_path, [], ['nxlink failed; --manual-wait 0'])
        say('  nxlink keeps failing. Start Downhill Revived on the console by hand, please (%d s; waiting for its FTP to go away)'
            % wait)
        self.ftp.delete(GAME_DIR + '/dhd.log')  # an earlier run's log must not read as this run's
        end = time.monotonic() + wait
        running = False
        while not running:
            downs = 0
            while downs < 3:
                if time.monotonic() > end:
                    return GameRun('manual', False, None, log_path, [], ['nxlink failed and nobody started the game in %d s' % wait])
                downs = 0 if self.ftp.alive(2) else downs + 1
                time.sleep(4)
            # the Wi-Fi drops for longer than that too: only the game's new log says it started
            say('  FTP went away; the game is probably running')
            came_back = self.ftp.wait_alive(timeout)
            if not came_back or self.ftp.size(GAME_DIR + '/dhd.log'):
                running = True
            else:
                say('  FTP is back and there is no new log: a Wi-Fi drop, still waiting for the game')
        say('  waited for sphaira to come back')
        ended = 'exit' if came_back else 'timeout'
        entries = []
        if ended == 'exit' and self.ftp.get(GAME_DIR + '/dhd.log', log_path):
            text = Path(log_path).read_text(encoding='utf-8', errors='replace')
            entries = [(None, line) for line in text.splitlines()]
        return GameRun('manual', True, ended, log_path, entries, ['started by hand; log from dhd.log on the card (no arrival times)'])

    def start_by_stub(self, folder, timeout):
        """nxlink sends only the small starter NRO, which starts the game on the card: nobody at the console."""
        cmd = [str(self.args.nxlink), '-a', self.args.ip, '-s', self.args.start_nro]  # -s: nxlink stays connected
        for k in range(1, self.args.nxlink_tries + 1):
            say('  starter over nxlink, try %d of %d' % (k, self.args.nxlink_tries))
            proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            gone = False
            for _ in range(30):
                if not self.ftp.alive(2):
                    gone = True
                    break
                time.sleep(2)
            proc.terminate()
            proc.wait()
            if gone:
                break
        return self.manual_launch(folder / 'game-manual.log', timeout)

    def launch(self, folder, timeout):
        if not self.ftp.wait_alive(self.args.ftp_wait):
            raise ConsoleGone('sphaira did not answer on FTP before the launch')
        notes = []
        if self.args.start_nro:
            return self.start_by_stub(folder, timeout)
        for k in range(1, self.args.nxlink_tries + 1):
            say('  nxlink try %d of %d' % (k, self.args.nxlink_tries))
            run = self.nxlink_once(folder / ('game-%d.log' % k), timeout)
            if run.launched:
                run.notes = notes + run.notes
                return run
            notes += ['nxlink try %d: %s' % (k, '; '.join(run.notes))]
            say('  ' + notes[-1])
            self.ftp.wait_alive(60)
            time.sleep(5)
        run = self.manual_launch(folder / 'game-manual.log', timeout)
        run.notes = notes + run.notes
        return run

    # ---- one game phase ----

    def play(self, phase, timeout):
        folder = self.out / phase.key
        folder.mkdir(parents=True, exist_ok=True)
        if self.console_gone:
            raise ConsoleGone('the console did not come back after an earlier phase')
        ini = self.ini_for(phase.key)
        (folder / 'dhd.ini').write_bytes(ini)
        for kind, _, name in self.ftp.list(GAME_DIR) or []:
            if kind != 'd' and re.fullmatch(r'cap_.*\.bmp', name):
                self.ftp.delete(GAME_DIR + '/' + name)
        self.ini_touched = True
        self.restore_note = 'a test file is on the card while the run goes on'
        self.ftp.put(ini, INI)
        start = time.monotonic()
        run = self.launch(folder, timeout)
        run.seconds = time.monotonic() - start
        phase.wall = run.seconds
        phase.analysis = parse_log(run.entries)
        (folder / 'bench.txt').write_text('\n'.join(phase.analysis['bench']) + '\n', encoding='utf-8')
        for note in run.notes:
            phase.details.append('- launch: ' + note)
        phase.details.append('- launched by %s, %s after %.0f s; log: `%s`' % (run.mode, run.ended or 'never started', run.seconds,
                                                                              rel(run.log)))
        if run.launched and run.ended == 'timeout' and not self.ftp.wait_alive(120):
            self.console_gone = True
            phase.details.append('- **the console did not come back** (a hung game or a crash screen): later phases are not run')
        if not self.console_gone:
            self.take_crashes(phase)
            self.take_captures(phase, folder)
            try:
                self.ftp.get(GAME_DIR + '/dhd.log', folder / 'dhd-card.log')
            except FtpError:
                pass
        return run

    def problems(self, phase, run):
        out = []
        if not run.launched:
            out.append('the game was not started: ' + '; '.join(run.notes))
        elif run.ended == 'timeout':
            out.append('timed out')
        a = phase.analysis
        if a['settings'] and 'NXTEST_PHASE=' + phase.key not in a['settings']:
            out.append('the log\'s settings line does not name this phase: its dhd.ini was not the one read')
        if a['hard']:
            out.append('%d serious error lines, first: %s' % (len(a['hard']), a['hard'][0]))
        if phase.crashes:
            out.append('%d new crash report(s)' % len(phase.crashes))
        return out

    def take_crashes(self, phase):
        for kind, size, name in self.ftp.list(CRASH_DIR) or []:
            if kind == 'd' or name in self.crash_seen:
                continue
            self.crash_seen.add(name)
            local = self.out / 'crash_reports' / name
            got = self.ftp.get(CRASH_DIR + '/' + name, local)
            phase.crashes.append((name, size, local if got else None))

    def take_captures(self, phase, folder):
        names = [n for kind, _, n in self.ftp.list(GAME_DIR) or [] if kind != 'd' and re.fullmatch(r'cap_.*\.bmp', n)]
        sips = shutil.which('sips')
        for name in sorted(names, key=capture_order):
            local = folder / 'captures' / name
            try:
                if not self.ftp.get(GAME_DIR + '/' + name, local):
                    continue
            except FtpError as e:
                phase.details.append('- capture %s not fetched: %s' % (name, e))
                continue
            kept = local
            if sips:
                png = local.with_suffix('.png')
                p = subprocess.run([sips, '-s', 'format', 'png', str(local), '--out', str(png)], capture_output=True)
                if p.returncode == 0 and png.exists():
                    local.unlink()
                    kept = png
            phase.captures.append(kept)
            self.ftp.delete(GAME_DIR + '/' + name)

    # ---- phases ----

    def phase_a(self, phase):
        lines = phase.details
        if not self.nro.exists():
            return phase.done('FAIL', 'no NRO at %s' % self.nro)
        lines.append('- NRO: `%s`, %d bytes, SHA-256 `%s`' % (rel(self.nro), self.nro.stat().st_size, self.nro_sha))
        lines += ['- ' + line for line in tool_versions(self.args)]
        try:
            p = subprocess.run(['ping', '-c', '3', self.args.ip], capture_output=True, text=True, timeout=30)
            lines.append('- ping: %s' % ((p.stdout or '').strip().splitlines()[-1:] or ['no answer'])[0])
        except (OSError, subprocess.TimeoutExpired):
            lines.append('- ping: no answer in 30 s')
        listing = self.ftp.list(GAME_DIR)
        if listing is None:
            return phase.done('FAIL', 'no %s on the card' % GAME_DIR)
        lines.append('- `%s`: %s' % (GAME_DIR, ', '.join('%s (%d)' % (n, s) for _, s, n in listing) or 'listed nothing'))
        iso = next((s for _, s, n in listing if n == ISO_NAME), None)
        if iso != ISO_SIZE:
            return phase.done('FAIL', '%s on the card is %s bytes, not %d' % (ISO_NAME, iso, ISO_SIZE))
        lines.append('- disc image size OK (%d)' % iso)
        old = next((s for _, s, n in listing if n == self.nro.name), None)
        lines.append('- NRO on the card before: %s bytes' % old)
        lines.append('- crash reports already on the card: %d' % len(self.crash_seen))
        clk = self.ftp.read(SYS_CLK)
        if clk is None:
            lines.append('- sys-clk: no `%s`' % SYS_CLK)
        else:
            (self.out / 'sys-clk-config.ini').write_bytes(clk)
            lines.append('- sys-clk `%s` (read only):\n\n```\n%s\n```' % (SYS_CLK, clk.decode('utf-8', 'replace').strip()))
        size = self.nro.stat().st_size
        target = GAME_DIR + '/' + self.nro.name
        tries = self.ftp.upload_resume(self.nro, target)
        lines.append('- uploaded in %d try(s)' % tries)
        length = min(65536, size)
        same_tail, _ = self.ftp.same_range(self.nro, GAME_DIR + '/' + self.nro.name, size - length, length)
        if same_tail:
            lines.append('- size and the last %d bytes match' % length)
        else:  # the server may not take REST on RETR: the whole file decides
            local = self.out / 'nro-readback.bin'
            self.ftp.get(GAME_DIR + '/' + self.nro.name, local)
            same = local.exists() and sha256(local) == self.nro_sha
            if local.exists():
                local.unlink()
            if not same:
                return phase.done('FAIL', 'the NRO on the card differs from the local one')
            lines.append('- tail range not usable on this server; the whole file read back and hashes the same')
        phase.done('PASS', 'console, disc image and NRO in place')

    def phase_b(self, phase):
        run = self.play(phase, self.args.menu_seconds + 180)
        problems = self.problems(phase, run)
        a = phase.analysis
        if run.launched and not a['rows'] and run.mode == 'nxlink':
            problems.append('no once-a-second lines in the stream')
        if run.launched and not phase.captures:
            problems.append('no captures')
        last = next((r['t'] for r in reversed(a['rows']) if r['t'] is not None), None)
        ran = last - a['rows'][0]['t'] if last is not None else None
        if run.launched and ran is not None and ran < self.args.menu_seconds - 15:
            problems.append('its log ends after %.0f s of %d' % (ran, self.args.menu_seconds))
        phase.details += time_series(a)
        phase.done('FAIL' if problems else 'PASS', '; '.join(problems) or
                   'about %s s logged, %d captures' % (num(ran, 0), len(phase.captures)))

    def phase_c(self, phase):
        run = self.play(phase, self.args.race_timeout)
        problems = self.problems(phase, run)
        a = phase.analysis
        if run.launched and not a['bench_main']:
            problems.append('no bench line: the game did not reach the end of the window')
        window = self.scenes['race'][1]
        phase.details.append('- window %s, sound on, paced (no DHD_FRAME_CLOCK), seed %s' % (window, self.scenes['race'][0]['DHD_RACE_SEED']))
        if a['bench']:
            phase.details.append('\nEvery bench line:\n\n```\n%s\n```' % '\n'.join(a['bench']))
        summary = race_summary(a)
        if summary:
            line = '- heartbeat after GO (field %d): median %.1f, lowest %.1f fields/s over %d lines' % (
                summary['go'], summary['median'], summary['low'], summary['lines'])
            if 'draws_per_picture' in summary:
                line += '; %.0f draws per picture' % summary['draws_per_picture']
            phase.details.append(line)
            if 'heavy' in summary:
                h = summary['heavy']
                line = '- heaviest stretch: fields %d-%d at %.1f fields/s' % (h['first'], h['last'], h['rate'])
                if h['draws_per_picture'] is not None:
                    line += ', %.0f draws per picture' % h['draws_per_picture']
                phase.details.append(line)
        else:
            phase.details.append('- heartbeat speed after GO: not available from this log')
        phase.details.append('- the captures come at pictures 120-2200 (each stops the renderer for an SD write), before this window')
        phase.done('FAIL' if problems else 'PASS', '; '.join(problems) or
                   'window done at %.1f fields/s (target %d)' % (a['bench_main'].get('fields_per_s', 0), TARGET_FPS))

    def phase_d(self, phase):
        card = TEST_DIR + '/memcard-card'
        self.ftp.remove_tree(card)
        before = {n for _, _, n in self.ftp.list(GAME_DIR) or []}
        run = self.play(phase, self.args.save_timeout)
        problems = self.problems(phase, run)
        if not run.launched:
            return phase.done('FAIL', '; '.join(problems))
        a = phase.analysis
        script = self.trace['SCENARIOS']['memcard'][0]
        steps = script.count(',') + 1
        reached = a['nav']
        phase.details.append('- menu script (%d steps), last step logged: %s\n\n```\n%s\n```' % (steps, reached, script))
        if self.console_gone:
            return phase.done('FAIL', '; '.join(problems))
        files, _ = self.ftp.walk(card)
        after = self.ftp.list(GAME_DIR) or []
        stray = [(n, s) for _, s, n in after if n not in before and n != TEST_DIR.rpartition('/')[2] and not re.fullmatch(r'cap_.*\.bmp', n)]
        for path, size in files:
            phase.details.append('- `%s`: %d bytes' % (path, size))
        if stray:
            phase.details.append('- new beside the game: %s' % ', '.join('%s (%d)' % x for x in stray))
        written = [f for f in files if f[1] > 0] + [x for x in stray if x[1] > 0]
        phase.details.append('- the load is checked only by the menu script reaching its last step without an error; '
                             'nothing compares the loaded data')
        if problems:
            return phase.done('FAIL', '; '.join(problems))
        if written and reached is not None and reached >= steps - 1:
            return phase.done('PASS', '%d non-empty save files; the script reached its last step (load: see details)' % len(written))
        if written:
            return phase.done('FAIL', 'save files exist, but the script stopped at step %s of %d: the load was not reached' % (reached, steps))
        if reached is None or reached < steps - 1:
            return phase.done('NOT RUN', 'not tested: without a trace the script stopped at step %s of %d, and nothing was saved' % (reached, steps))
        phase.done('FAIL', 'the script reached its last step, yet no save file with data under %s' % GAME_DIR)

    def phase_e(self, phase):
        reference = Path(self.args.reference)
        if not reference.exists():
            return phase.done('NOT RUN', 'no reference trace at %s' % reference)
        folder = self.out / 'E'
        folder.mkdir(parents=True, exist_ok=True)
        for name in ('trace.tsv', 'trace.ptrs.tsv'):
            self.ftp.delete(TEST_DIR + '/' + name)
        for name in ('state_trace_mask.txt', 'state_trace_codeptrs.txt'):
            self.ftp.put((TOOLS / name).read_bytes(), TEST_DIR + '/' + name)
        run = self.play(phase, self.args.trace_timeout)
        problems = self.problems(phase, run)
        if not run.launched or self.console_gone:
            return phase.done('FAIL', '; '.join(problems) or 'no trace')
        mine = folder / 'switch-race.tsv'
        if not self.ftp.get(TEST_DIR + '/trace.tsv', mine):
            return phase.done('FAIL', '; '.join(problems + ['no trace file on the card (a crash loses the buffered rows)']))
        try:
            self.ftp.get(TEST_DIR + '/trace.ptrs.tsv', folder / 'switch-race.ptrs.tsv')
        except FtpError:
            pass
        result = compare_traces(mine, reference, self.args.probe_field)
        for tool in ('trace_columns.py', 'state_diff.py'):
            p = subprocess.run([sys.executable, str(TOOLS / tool), str(mine), str(reference)], capture_output=True, text=True, timeout=300)
            text = (p.stdout + p.stderr).strip()
            (folder / (tool + '.txt')).write_text(text + '\n', encoding='utf-8')
            phase.details.append('\n`%s` (exit %d):\n\n```\n%s\n```' % (tool, p.returncode, clip(text.replace(str(ROOT) + '/', ''), 40)))
        table = ['| column | first differing field |', '|---|---:|']
        table += ['| %s | %s |' % (c, result['first'].get(c, 'none')) for c in result['columns']]
        phase.details += ['', 'Each column against the reference, %d rows here, %d there:' % (result['rows'], result['reference_rows']), ''] + table
        if result['env_diff']:
            phase.details.append('\n- recipe differences in the trace headers (here, reference): %s' % result['env_diff'])
        probe = self.args.probe_field
        phase.details += ['',
                          '- **Up to the first lens-flare Z probe (fields below %d)**: %s' % (
                              probe, 'FAIL' if result['early'] or not result['full_before'] else 'PASS'),
                          '- **The whole run**: %s' % ('PASS' if result['whole'] else 'FAIL'),
                          "- After about field %d the Switch may diverge legitimately: its exact renderer is a stub, so the "
                          "probes that DHD_EXACT_PROBE=1 asks of it do not answer as the reference's did." % probe,
                          '- The reference comes from a test-only Intel Mac build: this shows agreement with that build, not with retail.']
        (self.out / 'E' / 'recipe.txt').write_text(self.ini_for('E').decode(), encoding='utf-8')
        if result['early']:
            field, column = result['early'][0]
            problems.append('first difference before the probes: field %d, column %s' % (field, column))
        elif not result['full_before']:
            problems.append('the trace ends before field %d (%d rows)' % (probe, result['rows']))
        whole = 'the whole run matches' if result['whole'] else (
            'after that, from field %s (%s)' % (min(result['first'].values()), ', '.join(sorted(result['first'], key=result['first'].get)[:4]))
            if result['first'] else 'all shared rows match, %d of %d rows' % (result['rows'], result['reference_rows']))
        phase.done('FAIL' if problems else 'PASS', '; '.join(problems) if problems else 'matches up to field %d; %s' % (probe - 1, whole))

    def phase_f(self, phase):
        if self.console_gone:
            return phase.done('FAIL', 'the console did not come back: nothing to read')
        self.take_crashes(phase)
        got = self.ftp.get(GAME_DIR + '/dhd.log', self.out / 'dhd-final.log')
        phase.details.append('- final dhd.log: %s (nxlink runs do not write it, so it is from the last start without nxlink)' % (
            '`dhd-final.log`' if got else 'none on the card'))
        reports = [(p.key, c) for p in self.phases.values() for c in p.crashes]
        if reports:
            phase.details.append('\nNew crash reports:\n')
            for key, (name, size, local) in reports:
                phase.details.append('- phase %s: `%s`, %d bytes\n\n```\n%s\n```' % (key, name, size, crash_summary(local)))
        thermal = [line for p in self.phases.values() if p.analysis for line in p.analysis['thermal']]
        if got:
            for line in (self.out / 'dhd-final.log').read_text(encoding='utf-8', errors='replace').splitlines():
                if THERMAL.search(line):
                    thermal.append(line[:220])
        phase.details.append('- thermal: ' + ('\n\n```\n%s\n```' % '\n'.join(thermal[:20]) if thermal else 'nothing in the logs'))
        phase.done('FAIL' if reports else 'PASS', '%d new crash report(s)' % len(reports))

    # ---- around the phases ----

    def setup(self):
        """Before anything can be changed: the console answers, dhd.ini and the crash baseline are saved."""
        if not self.ftp.wait_alive(30):
            raise FtpError('no FTP answer from the console at port %d' % self.args.ftp_port)
        listing = self.ftp.list(GAME_DIR)
        if listing is None:
            raise FtpError('no %s on the card' % GAME_DIR)
        self.nro_sha = sha256(self.nro) if self.nro.exists() else None
        current = self.ftp.read(INI) if any(n == 'dhd.ini' for _, _, n in listing) else None
        if current is not None and current.startswith(MARKER):
            current = self.earlier_backup()
        if current is None:
            (self.out / 'dhd.ini.absent').write_text('the player had no dhd.ini\n')
        else:
            (self.out / 'dhd.ini.original').write_bytes(current)
        self.original = current
        self.backup_taken = True
        self.crash_seen = {n for kind, _, n in self.ftp.list(CRASH_DIR) or [] if kind != 'd'}
        try:
            self.ftp.get(GAME_DIR + '/dhd.log', self.out / 'dhd-before.log')
        except FtpError:
            pass

    def earlier_backup(self):
        """dhd.ini is one of ours, left by a run that was killed: the newest saved copy is the player's."""
        runs = sorted((p for p in (ROOT / 'build/nx-test').glob('*') if p != self.out and p.is_dir()), reverse=True)
        for run in runs:
            if (run / 'dhd.ini.original').exists():
                self.notes.append('dhd.ini on the card was a test file from an earlier run; the original is from `%s`' % rel(run))
                return (run / 'dhd.ini.original').read_bytes()
            if (run / 'dhd.ini.absent').exists():
                self.notes.append('dhd.ini on the card was a test file from an earlier run, which found none')
                return None
        raise FtpError('dhd.ini on the card is a test file and no copy of the original is saved here')

    def restore(self):
        if not self.backup_taken or not self.ini_touched:
            return
        say('Restoring the player\'s dhd.ini')
        end = time.monotonic() + (120 if self.console_gone else self.args.restore_wait)
        keep = self.out / ('dhd.ini.original' if self.original is not None else 'dhd.ini.absent')
        try:
            while True:
                try:
                    if self.ftp.wait_alive(30):
                        if self.original is None:
                            self.ftp.delete(INI)
                            if self.ftp.size(INI) is None:
                                self.restore_note = 'there was none; the test file is removed, checked'
                                return
                        else:
                            self.ftp.put(self.original, INI)  # reads it back
                            self.restore_note = 'restored and read back, the same bytes'
                            return
                except FtpError as e:
                    say('  not yet: %s' % e)
                if time.monotonic() > end:
                    break
                say('  the console does not answer yet (the game may still be running)')
        except KeyboardInterrupt:
            say('  stopped waiting')
        self.restore_note = '**NOT RESTORED**: the card still has a test file. The original is `%s`' % rel(keep)
        say('dhd.ini NOT restored: put %s back as %s on the card by hand (or run this tool again: it recognises its own file)'
            % (keep, INI))

    def cleanup(self):
        try:
            if not self.console_gone and self.ftp.alive():
                self.ftp.remove_tree(TEST_DIR)
        except (FtpError, KeyboardInterrupt) as e:
            say('test folder not removed: %s' % e)

    def cooldown(self):
        seconds = self.args.cooldown
        if self.launches and seconds > 0:
            say('Cooling down %d s' % seconds)
            time.sleep(seconds)
        self.launches += 1

    def run_phase(self, key):
        phase = self.phases[key]
        method = getattr(self, 'phase_' + key.lower())
        say('Phase %s: %s' % (key, phase.title))
        phase.reason = ''
        if self.console_gone and key != 'F':
            phase.done('NOT RUN', 'the console did not come back after an earlier phase')
            say('  NOT RUN: %s' % phase.reason)
            return
        try:
            if key in self.PHASE_ENV:
                self.cooldown()
            method(phase)
        except ConsoleGone as e:
            phase.done('NOT RUN', str(e))
            self.console_gone = True
        except FtpError as e:
            phase.done('FAIL', 'FTP: %s' % e)
        except Exception:
            phase.done('FAIL', 'the tool failed: %s' % traceback.format_exc().strip().splitlines()[-1])
            (self.out / ('%s-tool-error.txt' % key)).write_text(traceback.format_exc(), encoding='utf-8')
        say('  %s: %s' % (phase.status, phase.reason))
        self.write_report()

    def run(self):
        selected = self.args.only
        interrupted = False
        try:
            try:
                self.setup()
            except FtpError as e:
                say('Setup failed: %s' % e)
                for phase in self.phases.values():
                    phase.done('NOT RUN', 'setup failed: %s' % e)
                if 'A' in selected:
                    self.phases['A'].done('FAIL', str(e))
                return 1
            for key in selected:
                self.run_phase(key)
                if key == 'A' and self.phases['A'].status != 'PASS':
                    for other in selected:
                        if other != 'A':
                            self.phases[other].done('NOT RUN', 'preflight failed')
                    return 1
        except KeyboardInterrupt:
            interrupted = True
            say('\nInterrupted')
            for key in selected:
                if self.phases[key].status == 'NOT RUN' and self.phases[key].reason in ('', 'not selected (--only)'):
                    self.phases[key].done('NOT RUN', 'interrupted (Ctrl+C)')
        finally:
            self.restore()
            self.cleanup()
            self.write_report()
        return 1 if interrupted or any(p.status == 'FAIL' for p in self.phases.values()) else 0

    def dry_run(self):
        say('Dry run: nothing touches the network or the console.\n')
        self.nro_sha = sha256(self.nro) if self.nro.exists() else None
        lines = ['- NRO `%s`: %s' % (rel(self.nro), '%d bytes, SHA-256 %s' % (self.nro.stat().st_size, self.nro_sha) if self.nro_sha else 'not there'),
                 '- reference `%s`: %s' % (rel(self.args.reference), 'there' if Path(self.args.reference).exists() else 'not there')]
        lines += ['- ' + line for line in tool_versions(self.args)]
        self.phases['A'].details += lines
        for key in self.args.only:
            self.phases[key].done('NOT RUN', 'dry run')
            if key in self.PHASE_ENV:
                ini = self.ini_for(key)
                folder = self.out / key
                folder.mkdir(exist_ok=True)
                (folder / 'dhd.ini').write_bytes(ini)
                self.phases[key].details.append('dhd.ini of this phase:\n\n```\n%s```' % ini.decode())
                say('--- %s, dhd.ini\n%s' % (key, ini.decode()))
        if 'E' in self.args.only and Path(self.args.reference).exists():
            env, _, rows = load_trace(self.args.reference)
            base, wanted = self.env_e()
            diff = {k: (wanted.get(k), v) for k, v in env.items() if wanted.get(k) != v and k not in HEADER_IGNORED}
            self.phases['E'].details.append('- reference: %d rows; recipe against its header: %s' % (len(rows), diff or 'the same'))
            say('E: the reference has %d rows; recipe against its header: %s' % (len(rows), diff or 'the same'))
        self.write_report()
        return 0

    # ---- the report ----

    def write_report(self):
        (self.out / 'report.md').write_text(self.report(), encoding='utf-8')

    def report(self):
        out = ['# Switch full test, %s' % datetime.datetime.now().strftime('%Y-%m-%d %H:%M'), '']
        out += ['| phase | status | reason |', '|---|---|---|']
        for p in self.phases.values():
            out.append('| %s %s | %s | %s |' % (p.key, p.title, p.status, cell(p.reason)))
        out += ['', 'Player\'s dhd.ini: %s.' % self.restore_note]
        out += ['%s.' % n for n in self.notes]
        with_data = [p for p in self.phases.values() if p.analysis]
        out += ['', '## Speed (fields/s, target %d)' % TARGET_FPS, '']
        out += ['| scene | median, once-a-second lines | lowest | bench window | window fields/s | p50 ms | p99 ms | worst 1% ms | max ms |',
                '|---|---:|---:|---|---:|---:|---:|---:|---:|']
        for p in with_data:
            a = p.analysis
            rates = [r['rate'] for r in a['rows'] if r['rate'] is not None]
            if p.key == 'C' and race_summary(a):  # the race alone, after GO
                s = race_summary(a)
                median, low = s['median'], s['low']
            else:
                median, low = (statistics.median(rates), min(rates)) if rates else (None, None)
            b = a['bench_main'] or {}
            out.append('| %s | %s | %s | %s | %s | %s | %s | %s | %s |' % (
                p.title, num(median), num(low), b.get('window', '-'), num(b.get('fields_per_s')), num(b.get('p50_ms')), num(b.get('p99_ms')),
                num(b.get('worst1pct_ms')), num(b.get('max_ms'))))
        if not with_data:
            out.append('| no game phase ran | | | | | | | | |')
        out += ['', 'State trace and save runs use the virtual clock: their speed is not comparable with the target.']
        out += ['', '## Thread CPU (ms per field, `bench threads`)', '']
        threaded = [p for p in with_data if p.analysis['threads']]
        if threaded:
            names = sorted({k for p in threaded for k in p.analysis['threads']})
            out += ['| phase | ' + ' | '.join(names) + ' |', '|---|' + '---:|' * len(names)]
            for p in threaded:
                out.append('| %s | ' % p.key + ' | '.join(num(p.analysis['threads'].get(n)) for n in names) + ' |')
        else:
            out.append('No `bench threads` line.')
        out += ['', '## GPU and render timers', '']
        timed = [p for p in with_data if p.analysis['other'] or p.analysis['interp'] or p.analysis['snd']]
        for p in timed:
            a = p.analysis
            out.append('Phase %s, median of the once-a-second lines: %s' % (p.key, ', '.join('%s %s' % (k, num(v)) for k, v in sorted(a['other'].items())) or 'none'))
            out += ['- `%s`' % line for line in (a['interp'], a['snd']) if line]
            out.append('')
        if not timed:
            out.append('None in the logs.')
        out += ['## Errors', '']
        errors = [(p.key, e) for p in with_data for e in p.analysis['errors'].values()]
        for key, e in errors[:60]:
            out.append('- %s, %d x: `%s`' % (key, e['count'], e['first'].replace('`', "'")))
        if len(errors) > 60:
            out.append('- and %d more kinds; see the logs' % (len(errors) - 60))
        if not errors:
            out.append('None matched in the logs.')
        out += ['', '## Captures', '']
        shots = [(p.key, c) for p in self.phases.values() for c in p.captures]
        out += ['- %s: `%s`' % (key, rel(c)) for key, c in shots] or ['None.']
        for p in self.phases.values():
            if p.details:
                out += ['', '## Phase %s: %s' % (p.key, p.title), ''] + p.details
        return '\n'.join(out) + '\n'


def capture_order(name):
    m = re.match(r'cap_(\d+)_(.*)', name)
    return (int(m.group(1)), m.group(2)) if m else (0, name)


def time_series(a):
    rows = [r for r in a['rows'] if r['rate'] is not None]
    if not rows:
        return ['- fields/s over time: not available from this log']
    out = ['', 'Fields/s over time (every 5th line):', '', '| t | field | fields/s |', '|---:|---:|---:|']
    for r in rows[::5]:
        out.append('| %s | %d | %.1f |' % (num(r['t'] - rows[0]['t'], 0), r['field'], r['rate']))
    return out + ['']


def crash_summary(path, lines=30):
    if not path or not Path(path).exists():
        return '(not fetched)'
    text = Path(path).read_text(encoding='utf-8', errors='replace').splitlines()
    keep = [line for line in text if re.search(r'Title ID|Program|Result|Error|Exception|Abort|Reason|PC:|FAR|Registers|Firmware', line)]
    return '\n'.join((keep or text)[:lines])


def clip(text, lines):
    rows = text.splitlines()
    return '\n'.join(rows[:lines] + (['... %d more lines' % (len(rows) - lines)] if len(rows) > lines else []))


def cell(text):
    return str(text).replace('|', '/').replace('\n', ' ')


def num(value, digits=1):
    return '-' if value is None else ('%.*f' % (digits, value) if isinstance(value, (int, float)) else str(value))


def rel(path):
    try:
        return str(Path(path).resolve().relative_to(ROOT))
    except ValueError:
        return str(path)


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def tool_versions(args):
    def first(*cmd):
        try:
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=20)
            return (p.stdout or p.stderr).strip()
        except (OSError, subprocess.TimeoutExpired):
            return ''
    lines = ['git commit %s, %d files changed or new' % (first('git', '-C', str(ROOT), 'rev-parse', '--short', 'HEAD'),
                                                       len(first('git', '-C', str(ROOT), 'status', '--porcelain').splitlines())),
             'python %s; %s' % (sys.version.split()[0], first('curl', '--version').split('\n')[0])]
    packages = first('dkp-pacman', '-Q') or first(str(Path(os.environ.get('DEVKITPRO') or '/opt/devkitpro') / 'pacman/bin/pacman'), '-Q')
    wanted = [p for p in packages.splitlines() if re.match(r'(libnx|deko3d|uam|switch-tools|devkitA64) ', p)]
    lines.append('devkitPro: %s' % (', '.join(wanted) or 'versions not found'))
    return lines


def parse_only(text):
    letters = re.sub(r'[\s,]', '', text).upper()
    if not letters or set(letters) - set('ABCDEF'):
        raise argparse.ArgumentTypeError('phases are letters from ABCDEF')
    return [k for k in 'ABCDEF' if k in letters]


def main():
    kit = Path(os.environ.get('DEVKITPRO') or '/opt/devkitpro')
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--ip', default=os.environ.get('DHD_SWITCH_IP'), help="the console's address (or DHD_SWITCH_IP)")
    ap.add_argument('--nro', default=str(ROOT / 'build/switch/downhill-revived.nro'))
    ap.add_argument('--only', type=parse_only, default=list('ABCDEF'), help='phases to run, e.g. BC or A,C (default all)')
    ap.add_argument('--dry-run', action='store_true', help='no network: write each phase\'s dhd.ini and an empty report')
    ap.add_argument('--cooldown', type=int, default=60, help='seconds between launches, for the fan (default 60)')
    ap.add_argument('--out', help='folder (default build/nx-test/<timestamp>)')
    ap.add_argument('--ftp-port', type=int, default=5000)
    ap.add_argument('--nxlink', default=str(kit / 'tools/bin/nxlink'))
    ap.add_argument('--nxlink-tries', type=int, default=3)
    ap.add_argument('--send-timeout', type=int, default=420, help='seconds for nxlink to reach "server active"')
    ap.add_argument('--ftp-wait', type=int, default=180, help='seconds to wait for sphaira before a launch')
    ap.add_argument('--manual-wait', type=int, default=300, help='seconds for a person to start the game when nxlink keeps failing; 0: no fallback')
    ap.add_argument('--restore-wait', type=int, default=900, help='seconds to keep trying to restore dhd.ini')
    ap.add_argument('--menu-seconds', type=int, default=60)
    ap.add_argument('--race-timeout', type=int, default=600)
    ap.add_argument('--save-timeout', type=int, default=600)
    ap.add_argument('--trace-timeout', type=int, default=2700)
    ap.add_argument('--trace-fields', type=int, default=5000)
    ap.add_argument('--probe-field', type=int, default=1557, help='the first lens-flare Z probe: pass/fail is reported apart up to here')
    ap.add_argument('--reference', default=str(ROOT / 'build/state-trace/mac-race.tsv'))
    ap.add_argument('--start-nro', help='a starter NRO for nxlink to send; the game is started from the card')
    ap.add_argument('--no-capture', action='store_true', help='race phase: no BMP captures')
    ap.add_argument('--set', action='append', default=[], metavar='KEY=VALUE', help='race phase: one more dhd.ini line (repeatable)')
    ap.add_argument('--verbose', action='store_true', help="print the game's stream as well")
    args = ap.parse_args()
    if not args.dry_run and not args.ip:
        ap.error('--ip or DHD_SWITCH_IP names the console')
    args.ip = args.ip or 'CONSOLE_IP'

    def stop(signum, frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, stop)
    if hasattr(signal, 'SIGHUP'):
        signal.signal(signal.SIGHUP, stop)
    test = Test(args)
    code = test.dry_run() if args.dry_run else test.run()
    test.write_report()
    say('\nReport: %s' % (test.out / 'report.md'))
    return code


if __name__ == '__main__':
    sys.exit(main())
