#!/usr/bin/env python3
"""build_guard.py — Claude Code PreToolUse hook: refuse, before it runs, any
build command that would skip the machine-wide build jobserver, and say which
command to run instead.

WHY. 2026-09-30 13:16: a coordinator's lane briefs said
`cmake --build build --target bezel_tests_sema -j`. That is `make -j`, which
has no limit, and it bypassed dev_heavy.sh and the jobserver. Two lanes reached
499 concurrent clang, and the kernel OOM killer took three Claude sessions and
plasmashell. The real limit now lives in every compile rule
(scripts/build_token.sh), so even a bare `make -j` queues for tokens. This hook
is the layer in front of it. It keeps sessions on the supported path (the
dev_heavy.sh slice, a joined pool) and stops a drifting brief at dispatch time,
before a lane copies it.

What it denies (Bash commands, including `bash -c` payloads, `$( )`,
`eval`, heredocs fed to a shell, and wrappers like nohup/setsid/timeout/env/
systemd-run):
  * `cmake --build ...` not run as `scripts/dev_heavy.sh build cmake --build ...`
  * make/gmake/ninja in a CMake build dir (same rule)
  * `dev_heavy.sh build <wrapper> <build>`, e.g. `bash -c "cmake --build"`:
    dev_heavy joins the pool only when argv[0] is the build tool itself
  * `dev_heavy.sh probe|suite <build>`: builds belong in `build`
  * setting BEZEL_JOBSERVER to anything but on, MAKEFLAGS/MFLAGS/
    GNUMAKEFLAGS, BEZEL_BUILD_TOKEN_*, or passing --jobserver-auth
It also denies kills that pick processes by name or pattern, because several
sessions and ~20 lanes share this machine and such a kill takes everyone's
processes (2026-10-01: a lane's `pkill` by name killed other lanes' test runs):
  * `pkill` / `killall` matched by name, or by a pattern that is not a path
    inside one lane's own directory (e.g. `/mnt/fast/worktrees/<wt>/`)
  * `kill $(pgrep ...)`, `kill `pidof ...``, `pgrep ... | xargs kill`, and
    `kill $p` where `p` came from `pgrep`/`pidof`/`ps` in the same command
  * `kill -1` / `kill -- -1` (every process the user owns)
`kill <PID>`, `kill -0 <PID>`, `kill %job`, `kill $!`, `pkill -P <ppid>`,
`pkill -F <pidfile>` and a path-scoped `pkill -f /mnt/fast/worktrees/<wt>/` stay
allowed.
Agent / SendMessage prompts and Write/Edit of a *brief* file are linted for the
same commands written as instructions. A line that says "never ..." is left
alone.

It never allows anything (no permissionDecision "allow"): a command it lets
through goes through the normal permission flow. Any internal error lets the
command through; the launcher still bounds the build.

Install: settings.json PreToolUse, matcher "Bash|Agent|Task|SendMessage|Write|Edit",
command `python3 <copy of this file> --root <repo> [--root <dir> ...]`.
It applies only when the session's cwd (or, for Bash, the command text) is under
a --root, so other projects are unaffected.
"""
from __future__ import annotations

import json
import os
import re
import sys
import time

BUILD_TOOLS = {"make", "gmake", "ninja", "ninja-build"}
SHELLS = {"bash", "sh", "zsh", "dash", "ksh"}
ASSIGN = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)=(.*)$", re.S)
MAX_DEPTH = 6
DENY_LOG = "/var/tmp/bezel-scratch/build-guard/denials.log"

# Wrappers that run their operand as a command: {name: options taking a value}.
WRAPPERS = {
    "env": {"-u", "--unset", "-C", "--chdir", "-S", "--split-string"},
    "nice": {"-n", "--adjustment"},
    "ionice": {"-c", "--class", "-n", "--classdata"},
    "nohup": set(),
    "setsid": set(),
    "time": {"-f", "--format", "-o", "--output"},
    "command": set(),
    "builtin": set(),
    "exec": {"-a"},
    "stdbuf": {"-i", "--input", "-o", "--output", "-e", "--error"},
    "timeout": {"-s", "--signal", "-k", "--kill-after"},
    "taskset": set(),
    "chrt": set(),
    "sudo": {"-u", "--user", "-g", "--group", "-C", "-D", "--chdir"},
    "systemd-run": {"-p", "--property", "-E", "--setenv", "-u", "--unit",
                    "--slice", "--description", "-H", "--host", "-M", "--machine",
                    "--uid", "--gid", "--nice", "--working-directory", "-t",
                    "--on-active", "--on-calendar", "--timer-property", "--path-property",
                    "--socket-property", "--service-type"},
    "xargs": {"-a", "-E", "-e", "-I", "-i", "-L", "-l", "-n", "-P", "-s", "-d"},
    "flock": {"-w", "--timeout", "-E", "--conflict-exit-code", "-c", "--command"},
    "caffeinate": set(),
    "watch": {"-n", "--interval", "-d"},
    # Shell keywords that start a command list: `if cmake ...; then make ...`.
    **{kw: set() for kw in ("if", "then", "else", "elif", "while", "until", "do", "!", "{")},
}
# After options, these take positional words before the command.
WRAPPER_POSITIONALS = {"timeout": 1, "taskset": 1, "chrt": 1, "flock": 1}

# Kills. Name-matching killers, the commands that list PIDs by name, and the
# pkill options that select by PID (which are scoped and so allowed).
NAME_KILLERS = {"killall", "skill", "slay"}
PID_FINDERS = {"pgrep", "pidof", "ps", "lsof", "fuser", "pstree"}
PKILL_VALUE_OPTS = set("dgGPstuUFOrq")  # short options that take a value
PKILL_LONG_VALUE_OPTS = {"--delimiter", "--pgroup", "--group", "--parent", "--session",
                         "--terminal", "--euid", "--uid", "--pidfile", "--older",
                         "--runstates", "--queue", "--signal", "--ns", "--nslist",
                         "--cgroup", "--env"}
PKILL_PID_SCOPES = {"-P", "--parent", "-F", "--pidfile", "-s", "--session", "-g", "--pgroup"}
# Directories under a --root that hold state every lane shares: a pattern
# inside them is not one lane's own.
PS_PID_SCOPES = {"-p", "--pid", "-q", "--quick-pid", "--ppid"}
SHARED_DIRS = {"aot-harness", "aot-bincache", "ccache", "build", "wt-jobserver",
               "jobserver", "tmp"}
KILL_REFUSAL = ("build_guard: refused, this kill selects processes by name or pattern. Several "
                "Claude sessions and ~20 lanes share this machine, so it would also kill other "
                "lanes' builds and test runs (2026-10-01 incident).")
KILL_INSTEAD = ("list the candidates first (`pgrep -af <pattern>`), keep only PIDs that "
                "belong to your own lane, then `kill <PID>` (then `kill -9 <PID>` after 2 s); "
                "or match your own lane's path: "
                "pkill -f \"$(readlink -f /mnt/fast/worktrees/<your-wt>/build)/\"")


def _base(word: str) -> str:
    return os.path.basename(word.rstrip("/"))


# ---------------------------------------------------------------- tokenizer --

def _substitutions(word: str) -> list[str]:
    """The bodies of the $( ) and `...` substitutions inside one word."""
    out: list[str] = []
    i, n = 0, len(word)
    while i < n:
        if word.startswith("$(", i) and not word.startswith("$((", i):
            k = _match_close(word, i + 1)
            out.append(word[i + 2:k])
            i = k + 1
        elif word[i] == "`":
            k = word.find("`", i + 1)
            k = n if k < 0 else k
            out.append(word[i + 1:k])
            i = k + 1
        else:
            i += 1
    return out


def _match_close(s: str, i: int) -> int:
    """s[i] == '(' -> index of the matching ')' (quote-aware), or len(s)."""
    depth = 0
    j = i
    n = len(s)
    while j < n:
        c = s[j]
        if c == "\\":
            j += 2
            continue
        if c == "'":
            k = s.find("'", j + 1)
            j = n if k < 0 else k + 1
            continue
        if c == '"':
            j += 1
            while j < n and s[j] != '"':
                j += 2 if s[j] == "\\" else 1
            j += 1
            continue
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return j
        j += 1
    return n


HEREDOC = re.compile(r"<<(?!<)-?\s*(['\"]?)([A-Za-z_][A-Za-z0-9_]*)\1")


def _split_heredocs(text: str) -> tuple[str, list[str]]:
    """Drop heredoc bodies from `text`. Return (text without bodies, bodies that
    feed a shell, which are commands too)."""
    lines = text.split("\n")
    out: list[str] = []
    shell_bodies: list[str] = []
    i = 0
    while i < len(lines):
        line = lines[i]
        out.append(line)
        i += 1
        delims = [(m.group(0).startswith("<<-"), m.group(2)) for m in HEREDOC.finditer(line)]
        if not delims:
            continue
        feeds_shell = bool(re.search(r"(^|[\s;&|(])(\S*/)?(bash|sh|zsh|dash|ksh)(\s+-[A-Za-z]+)*\s*(<<|$)",
                                     line.split("<<")[0] + "<<"))
        for strip_tabs, delim in delims:
            body: list[str] = []
            while i < len(lines):
                cand = lines[i].lstrip("\t") if strip_tabs else lines[i]
                i += 1
                if cand == delim:
                    break
                body.append(lines[i - 1])
            if feeds_shell:
                shell_bodies.append("\n".join(body))
    return "\n".join(out), shell_bodies


def tokenize(s: str) -> tuple[list[list[str]], list[str]]:
    """Approximate POSIX shell word splitting. Returns (simple commands as word
    lists, nested command strings from $( ), backticks and <( ))."""
    cmds: list[list[str]] = []
    nested: list[str] = []
    words: list[str] = []
    cur: list[str] | None = None
    skip_word = False  # the next word is a redirection target
    n = len(s)
    i = 0

    def end_word():
        nonlocal cur, skip_word
        if cur is not None:
            if skip_word:
                skip_word = False
            else:
                words.append("".join(cur))
            cur = None

    def end_cmd():
        end_word()
        if words:
            cmds.append(words[:])
            words.clear()

    def add(text):
        nonlocal cur
        if cur is None:
            cur = []
        cur.append(text)

    while i < n:
        c = s[i]
        if c == "\\":
            if i + 1 < n and s[i + 1] == "\n":
                i += 2
                continue
            add(s[i + 1:i + 2])
            i += 2
            continue
        if c == "'":
            k = s.find("'", i + 1)
            k = n if k < 0 else k
            add(s[i + 1:k])
            i = k + 1
            continue
        if c == '"':
            j = i + 1
            buf = []
            while j < n and s[j] != '"':
                if s[j] == "\\" and j + 1 < n:
                    buf.append(s[j + 1])
                    j += 2
                    continue
                if s.startswith("$(", j) and not s.startswith("$((", j):
                    k = _match_close(s, j + 1)
                    nested.append(s[j + 2:k])
                    buf.append(s[j:k + 1])
                    j = k + 1
                    continue
                if s[j] == "`":
                    k = s.find("`", j + 1)
                    k = n if k < 0 else k
                    nested.append(s[j + 1:k])
                    buf.append(s[j:k + 1])
                    j = k + 1
                    continue
                buf.append(s[j])
                j += 1
            add("".join(buf))
            i = j + 1
            continue
        if c == "$" and s.startswith("$(", i) and not s.startswith("$((", i):
            k = _match_close(s, i + 1)
            nested.append(s[i + 2:k])
            add(s[i:k + 1])
            i = k + 1
            continue
        if c == "`":
            k = s.find("`", i + 1)
            k = n if k < 0 else k
            nested.append(s[i + 1:k])
            add(s[i:k + 1])  # keep it in the word, like $( ): `kill `pgrep x``
            i = k + 1
            continue
        if c in "<>":
            if i + 1 < n and s[i + 1] == "(":
                k = _match_close(s, i + 1)
                nested.append(s[i + 2:k])
                i = k + 1
                continue
            if cur is not None and "".join(cur).isdigit():
                cur = None  # an fd number: `2>&1`
            end_word()
            j = i
            while j < n and s[j] in "<>&|-":
                j += 1
            op = s[i:j]
            i = j
            if op.endswith("&") and i < n and (s[i].isdigit() or s[i] == "-"):
                while i < n and (s[i].isdigit() or s[i] == "-"):
                    i += 1
                continue
            skip_word = True
            while i < n and s[i] in " \t":
                i += 1
            continue
        if c in ";&|()\n":
            end_cmd()
            i += 1
            continue
        if c in " \t\r":
            end_word()
            i += 1
            continue
        if c == "#" and cur is None:
            k = s.find("\n", i)
            i = n if k < 0 else k
            continue
        add(c)
        i += 1
    end_cmd()
    return cmds, nested


# ----------------------------------------------------------------- analysis --

class Guard:
    def __init__(self, cwd: str, roots: list[str] | None = None):
        self.cwd = cwd
        self.vcwd = cwd
        self.roots = roots or []
        self.reasons: list[str] = []
        self.kill_reasons: list[str] = []
        self.pid_vars: set[str] = set()  # shell variables holding pgrep/pidof output
        self.unscoped_finder = False  # this script runs a by-name pgrep/pidof/ps

    def deny(self, why: str, instead: str | None = None):
        msg = why if not instead else f"{why}\n  Run instead: {instead}"
        if msg not in self.reasons:
            self.reasons.append(msg)

    def deny_kill(self, why: str):
        msg = f"{why}\n  Run instead: {KILL_INSTEAD}"
        if msg not in self.kill_reasons:
            self.kill_reasons.append(msg)

    # -- entry --------------------------------------------------------------
    def check_script(self, text: str, depth: int = 0):
        if depth > MAX_DEPTH or not text:
            return
        text, shell_bodies = _split_heredocs(text)
        cmds, nested = tokenize(text)
        outer = self.unscoped_finder
        for k, words in enumerate(cmds):
            # Only a listing that runs before an `xargs kill` can feed it.
            self.unscoped_finder = outer or self.finds_by_name(cmds[:k])
            self.check_words(words, depth)
        self.unscoped_finder = outer
        for sub in nested + shell_bodies:
            self.check_script(sub, depth + 1)

    # -- one simple command --------------------------------------------------
    def check_words(self, words: list[str], depth: int):
        for w in words:
            self.check_tamper(w)
            m = ASSIGN.match(w)
            if m and self.lists_pids(m.group(2)):
                self.pid_vars.add(m.group(1))
        if len(words) >= 4 and words[0] == "for" and words[2] == "in" and \
                any(self.lists_pids(w) for w in words[3:]):
            self.pid_vars.add(words[1])
        c = self.command_index(words, 0)
        if c is None:
            return
        name = _base(words[c])
        args = words[c + 1:]
        if name in ("kill", "pkill") or name in NAME_KILLERS:
            self.check_kill(name, args, via_xargs=any(_base(w) == "xargs" for w in words[:c]))
            return
        if name in ("cd", "pushd") and c == 0:
            self.change_dir(args)
            return
        if name in ("export", "declare", "typeset", "local", "readonly"):
            return  # tamper checks above cover the assignments
        if name == "eval":
            self.check_script(" ".join(args), depth + 1)
            return
        if name in SHELLS:
            payload = self.shell_payload(args)
            if payload is not None:
                self.check_script(payload, depth + 1)
            return
        if name == "dev_heavy.sh":
            self.check_dev_heavy(args, depth)
            return
        self.check_build(words[c:], raw=True)

    def check_tamper(self, word: str):
        m = ASSIGN.match(word)
        if m:
            var, val = m.group(1), m.group(2)
            if var == "BEZEL_JOBSERVER" and val not in ("", "on"):
                self.deny(f"`{word}` opts a build out of the machine-wide jobserver. Only "
                          "gate.sh may do that (it runs with zero other builders).",
                          "the same command without the assignment")
            elif var in ("MAKEFLAGS", "MFLAGS", "GNUMAKEFLAGS"):
                self.deny(f"`{var}=` overrides the jobserver auth that dev_heavy.sh sets.",
                          "the same command without the assignment; dev_heavy.sh sets MAKEFLAGS")
            elif var.startswith("BEZEL_BUILD_TOKEN_"):
                self.deny(f"`{var}=` changes the build token launcher's limits.",
                          "the same command without the assignment")
        if "--jobserver-auth" in word:
            self.deny("`--jobserver-auth` hand-wires a jobserver. dev_heavy.sh does that.",
                      "scripts/dev_heavy.sh build <cmake|make|ninja> ...")

    def command_index(self, words: list[str], i: int) -> int | None:
        """Index of the command word, skipping assignments and wrappers."""
        n = len(words)
        while i < n:
            w = words[i]
            if ASSIGN.match(w):
                i += 1
                continue
            name = _base(w)
            if name not in WRAPPERS:
                return i
            if name in ("taskset", "ionice") and i + 1 < n and words[i + 1] in ("-p", "--pid"):
                return None
            takes = WRAPPERS[name]
            i += 1
            while i < n:
                a = words[i]
                if a == "--":
                    i += 1
                    break
                if name == "env" and ASSIGN.match(a):
                    i += 1
                    continue
                if not a.startswith("-") or a == "-":
                    break
                i += 2 if (a in takes and "=" not in a) else 1
            i += WRAPPER_POSITIONALS.get(name, 0)
        return None

    @staticmethod
    def shell_payload(args: list[str]) -> str | None:
        for k, a in enumerate(args):
            if a.startswith("-") and not a.startswith("--") and "c" in a[1:]:
                return args[k + 1] if k + 1 < len(args) else None
            if not a.startswith("-"):
                return None  # a script file: out of reach, the launcher still bounds it
        return None

    def change_dir(self, args: list[str]):
        target = next((a for a in args if not a.startswith("-") or a == "-"), None)
        if target is None:
            self.vcwd = os.path.expanduser("~")
        elif target == "-":
            return
        else:
            self.vcwd = self.resolve(target, self.vcwd)

    @staticmethod
    def resolve(path: str, base: str) -> str:
        path = os.path.expandvars(os.path.expanduser(path))
        return os.path.normpath(os.path.join(base, path))

    # -- kills -----------------------------------------------------------------
    def lists_pids(self, word: str, depth: int = 0) -> bool:
        """`word` holds a $( ) or `` ` `` substitution that lists PIDs by name
        (pgrep/pidof/ps not scoped to one lane's path)."""
        if depth > MAX_DEPTH:
            return False
        for inner in _substitutions(word):
            text, _ = _split_heredocs(inner)
            cmds, nested = tokenize(text)
            if self.finds_by_name(cmds):
                return True
            if any(self.lists_pids(f"$({n})", depth + 1) for n in nested):
                return True
        return False

    def finds_by_name(self, cmds: list[list[str]]) -> bool:
        """Some command in `cmds` lists PIDs by name. A finder is scoped when it is
        `pgrep` with a -P/-F/-s/-g scope or a lane-path pattern, or a `ps` whose
        output a `grep` on a lane path filters."""
        finders, lane_grep = [], False
        for ws in cmds:
            c = self.command_index(ws, 0)
            if c is None:
                continue
            name, args = _base(ws[c]), ws[c + 1:]
            if name in ("grep", "egrep") and self.lane_scoped_path(self.grep_pattern(args) or ""):
                lane_grep = True
            elif name in PID_FINDERS:
                finders.append((name, args))
        for name, args in finders:
            if name == "pgrep":
                pattern, pid_scoped = self.pkill_pattern(args)
                if pid_scoped or (pattern is not None and self.lane_scoped_path(pattern)):
                    continue
            elif name == "ps" and (lane_grep or any(
                    a in PS_PID_SCOPES or re.match(r"^-[pq]\d", a) or a.split("=")[0] in PS_PID_SCOPES
                    for a in args)):
                continue  # filtered to one lane, or `ps -p <PIDs>`
            return True
        return False

    @staticmethod
    def grep_pattern(args: list[str]) -> str | None:
        for k, a in enumerate(args):
            if a in ("-e", "--regexp") and k + 1 < len(args):
                return args[k + 1]
            if a.startswith("--regexp="):
                return a.split("=", 1)[1]
            if not a.startswith("-"):
                return a
        return None

    def check_kill(self, name: str, args: list[str], via_xargs: bool):
        if name in NAME_KILLERS:
            self.deny_kill(f"`{name}` kills every process with that name on the machine, "
                           "including other lanes' and other sessions'.")
            return
        if name == "pkill":
            self.check_pkill(args)
            return
        # kill [-s SIG | -n NUM | -SIG] [--] target...
        targets: list[str] = []
        signal = None
        i = 0
        while i < len(args):
            a = args[i]
            if a == "--":
                targets.extend(args[i + 1:])
                break
            if a in ("-l", "-L", "--list", "--table"):
                return
            if a in ("-s", "-n", "--signal"):
                signal = args[i + 1] if i + 1 < len(args) else ""
                i += 2
                continue
            if a.startswith("-") and signal is None and not targets:
                signal = a[1:]  # -9, -TERM, -0
                i += 1
                continue
            targets.append(a)
            i += 1
        if signal == "0":
            return  # `kill -0` only probes whether the PID is alive
        if via_xargs:
            if self.unscoped_finder:
                self.deny_kill("`xargs kill` kills whatever PIDs the pipe produced; fed from "
                               "pgrep/ps/grep that is a by-name kill across every lane.")
            return
        for t in targets:
            if t == "-1":
                self.deny_kill("`kill -1` signals every process you own, in every session.")
            elif self.lists_pids(t):
                self.deny_kill(f"`kill {t}` kills every process the PID listing matches by "
                               "name, including other lanes' and other sessions'.")
            else:
                m = re.match(r'^"?\$\{?([A-Za-z_][A-Za-z0-9_]*)', t)
                if m and m.group(1) in self.pid_vars:
                    self.deny_kill(f"`kill {t}`: `${m.group(1)}` holds a by-name PID listing "
                                   "(pgrep/pidof/ps), which matches other lanes' processes.")

    def check_pkill(self, args: list[str]):
        pattern, pid_scoped = self.pkill_pattern(args)
        if pid_scoped:
            return
        if pattern is None:
            self.deny_kill("`pkill` with no pattern and no -P/-F/-s/-g PID scope matches "
                           "processes machine-wide.")
            return
        if not self.lane_scoped_path(pattern):
            self.deny_kill(f"`pkill ... {pattern}` matches by name or pattern across every "
                           "lane and session on this machine. Only a path inside one lane's own "
                           "directory (ending in `/`) is scoped.")

    @staticmethod
    def pkill_pattern(args: list[str]) -> tuple[str | None, bool]:
        """(pattern, scoped by -P/-F/-s/-g) from pkill/pgrep arguments."""
        pattern = None
        pid_scoped = False
        i = 0
        while i < len(args):
            a = args[i]
            if a == "--":
                if i + 1 < len(args) and pattern is None:
                    pattern = args[i + 1]
                break
            if a.startswith("--"):
                opt = a.split("=", 1)[0]
                if opt in PKILL_PID_SCOPES:
                    pid_scoped = True
                i += 2 if (opt in PKILL_LONG_VALUE_OPTS and "=" not in a) else 1
                continue
            if a.startswith("-") and len(a) > 1:
                body = a[1:]
                if body.isdigit() or (body.isupper() and len(body) > 1):
                    i += 1  # a signal: -9, -KILL
                    continue
                step = 1
                for k, ch in enumerate(body):
                    if ch in PKILL_VALUE_OPTS:
                        if f"-{ch}" in PKILL_PID_SCOPES:
                            pid_scoped = True
                        if k == len(body) - 1:
                            step = 2  # the value is the next word
                        break
                i += step
                continue
            if pattern is None:
                pattern = a
            i += 1
        return pattern, pid_scoped

    def lane_scoped_path(self, pattern: str) -> bool:
        """The pattern requires a path inside one lane's own directory: under a
        --root, not in a shared dir, and the lane-level component is closed by
        `/`, so `.../a24-wt-5188` cannot also match `.../a24-wt-5188-rv`. Plain
        text may precede the path (`codegen_half.sh /mnt/fast/worktrees/x/`);
        alternation and optional groups would let the pattern match without
        the path, so they are refused."""
        if "|" in pattern:
            return False
        p = pattern.lstrip("^")
        m = re.match(r"^\$\((?:readlink\s+-f|realpath)\s+([^)]+)\)(.*)$", p)
        if m:
            inner = self.resolve(m.group(1).strip().strip("'\""), self.vcwd)
            p = os.path.realpath(inner) + m.group(2)
        p = re.sub(r"^(\$PWD|\$\{PWD\}|\$\(pwd(?:\s+-P)?\))", lambda _: self.vcwd, p)
        for r in self.roots:
            for root in {r.rstrip("/"), os.path.realpath(r).rstrip("/")}:
                at = p.find(root + "/")
                # Only plain text before the path: no quantifier, group or class
                # that could make the path optional.
                if at < 0 or re.search(r"[*+?\[\](){}\\$]", p[:at]):
                    continue
                rel = p[at + len(root) + 1:]
                parts = rel.split("/")
                if parts[0] == ".claude":  # <repo>/.claude/worktrees/<wt>/
                    parts = parts[1:]
                # `worktrees/<wt>/...`: at least two components, the second closed by
                # a `/` that no quantifier makes optional.
                if len(parts) >= 3 and parts[0] and parts[1] and parts[0] not in SHARED_DIRS \
                        and not re.search(r"[.*+?\[\]()|{}^$\\]", parts[0] + parts[1]) \
                        and not re.match(r"[*?{]", parts[2]):
                    return True
        # A relative `worktrees/<wt>/` or `lane-builds/<wt-hash>/` names one lane too.
        for m in re.finditer(r"(?:^|(?<=[/\s]))(?:worktrees|lane-builds)/([^/]+)/(.?)", p):
            if not re.search(r"[*+?\[\](){}\\$]", p[:m.start()]) \
                    and not re.search(r"[.*+?\[\]()|{}^$\\]", m.group(1)) \
                    and m.group(2) not in ("*", "?", "{"):
                return True
        return False

    # -- dev_heavy.sh --------------------------------------------------------
    def check_dev_heavy(self, args: list[str], depth: int):
        if not args:
            return
        kind, payload = args[0], args[1:]
        if not payload:
            return
        pname = _base(payload[0])
        if kind == "build":
            if pname in BUILD_TOOLS or pname == "cmake":
                self.check_build(payload, raw=False)
                return
            # argv[0] is not the build tool: dev_heavy cannot join the pool.
            if self.contains_build(payload, depth):
                self.deny("`dev_heavy.sh build` joins the jobserver only when the build tool "
                          f"comes right after `build`; here it is `{payload[0]}`, so the build "
                          "runs with a private pool.",
                          "scripts/dev_heavy.sh build cmake --build <dir> [--target <t>] "
                          "(or scripts/agent_check.sh --area <group> --build-only)")
            return
        if self.contains_build(payload, depth):
            self.deny(f"`dev_heavy.sh {kind}` does not join the jobserver; builds go through "
                      "`dev_heavy.sh build`.",
                      "scripts/dev_heavy.sh build cmake --build <dir> [--target <t>] "
                      "(or scripts/agent_check.sh --area <group> --build-only)")

    def contains_build(self, words: list[str], depth: int) -> bool:
        probe = Guard(self.cwd, self.roots)
        probe.vcwd = self.vcwd
        probe.check_words(words, depth + 1)
        return bool(probe.reasons)

    # -- the build itself ----------------------------------------------------
    @staticmethod
    def compiles_nothing(argv: list[str]) -> bool:
        """A dry run (`make -n`, `cmake --build ... -- -n`), a ninja tool
        (`ninja -t targets`), --help/--version, or only help/clean targets."""
        name = _base(argv[0])
        args = argv[1:]
        if name == "cmake":
            if "--help" in args or "--version" in args:
                return True
            targets = [args[i + 1] for i, a in enumerate(args[:-1]) if a in ("--target", "-t")]
            targets += [t for a in args if a.startswith("--target=") for t in a[9:].split(",")]
            if targets and all(t in ("help", "clean") for t in targets):
                return True
            args = args[args.index("--") + 1:] if "--" in args else []
        elif name.startswith("ninja") and "-t" in args:
            return True
        else:
            valued = {"-C", "--directory", "-f", "--file", "--makefile", "-j", "-l", "-d", "-k",
                      "-o", "-W", "-I"}
            targets = [a for i, a in enumerate(args)
                       if not a.startswith("-") and "=" not in a and not (i and args[i - 1] in valued)]
            if targets and all(t in ("help", "clean") for t in targets):
                return True
        for a in args:
            if a in ("--dry-run", "--just-print", "--recon", "--question", "--help", "--version"):
                return True
            if re.fullmatch(r"-[a-zA-Z]*[nq][a-zA-Z]*", a) and not a.startswith(("-j", "-l")):
                return True
        return False

    def check_build(self, argv: list[str], raw: bool):
        name = _base(argv[0])
        if self.compiles_nothing(argv):
            return
        if name == "cmake":
            if "--build" not in argv[1:]:
                return  # configure, -E, --install: not a compile wave
            if raw:
                self.deny("`cmake --build` outside `scripts/dev_heavy.sh build` bypasses the "
                          "machine-wide jobserver and the heavy slice (a bare `-j` is "
                          "unlimited `make -j`: 499 clang and a box-wide OOM on 2026-09-30).",
                          self.cmake_instead(argv))
        elif name in BUILD_TOOLS:
            d = self.build_tool_dir(argv)
            if raw and self.is_cmake_build_dir(d):
                self.deny(f"`{name}` in the CMake build dir {d} bypasses the machine-wide "
                          "jobserver and the heavy slice.",
                          f"{self.dev_heavy_for(d)} build {' '.join(self.strip_j(argv))}")

    @staticmethod
    def strip_j(argv: list[str]) -> list[str]:
        out: list[str] = []
        i = 0
        while i < len(argv):
            a = argv[i]
            if a in ("-j", "--jobs", "--parallel"):
                if i + 1 < len(argv) and argv[i + 1].isdigit():
                    i += 1
            elif re.fullmatch(r"-j\d+|--jobs=\d*|--parallel=\d*|-l\d*(\.\d+)?", a):
                pass
            else:
                out.append(a)
            i += 1
        return out

    def cmake_instead(self, argv: list[str]) -> str:
        targets: list[str] = []
        bdir = None
        i = 1
        while i < len(argv):
            a = argv[i]
            if a == "--build" and i + 1 < len(argv):
                bdir = argv[i + 1]
                i += 2
                continue
            if a in ("--target", "-t"):
                i += 1
                while i < len(argv) and not argv[i].startswith("-"):
                    targets.append(argv[i])
                    i += 1
                continue
            if a.startswith("--target="):
                targets.extend(a.split("=", 1)[1].split(","))
            i += 1
        tests = [t for t in targets if re.fullmatch(r"bezel_tests(_\w+)?", t)]
        if targets and len(tests) == len(targets):
            areas = sorted({t[len("bezel_tests_"):] for t in tests if t != "bezel_tests"})
            if "bezel_tests" in tests or not areas:
                return "scripts/agent_check.sh --build-only"
            return " && ".join(f"scripts/agent_check.sh --area {a} --build-only" for a in areas)
        dh = self.dev_heavy_for(self.resolve(bdir, self.vcwd) if bdir else None)
        return f"{dh} build {' '.join(self.strip_j(argv))}"

    def build_tool_dir(self, argv: list[str]) -> str:
        d = self.vcwd
        mk = None
        i = 1
        while i < len(argv):
            a = argv[i]
            if a in ("-C", "--directory") and i + 1 < len(argv):
                d = self.resolve(argv[i + 1], d)
                i += 2
                continue
            if a.startswith("--directory="):
                d = self.resolve(a.split("=", 1)[1], d)
            elif a.startswith("-C") and len(a) > 2:
                d = self.resolve(a[2:], d)
            elif a in ("-f", "--file", "--makefile") and i + 1 < len(argv):
                mk = argv[i + 1]
                i += 2
                continue
            elif a.startswith(("--file=", "--makefile=")):
                mk = a.split("=", 1)[1]
            i += 1
        if mk:
            d = os.path.dirname(self.resolve(mk, d)) or d
        return d

    @staticmethod
    def is_cmake_build_dir(d: str) -> bool:
        if "$" in d:
            return "build" in d
        p = d
        for _ in range(5):
            if os.path.isfile(os.path.join(p, "CMakeCache.txt")):
                return True
            parent = os.path.dirname(p)
            if parent == p:
                break
            p = parent
        return False

    @staticmethod
    def dev_heavy_for(bdir: str | None) -> str:
        """Absolute dev_heavy.sh of the tree that owns `bdir`, if known."""
        p = bdir
        for _ in range(5):
            if not p:
                break
            cache = os.path.join(p, "CMakeCache.txt")
            if os.path.isfile(cache):
                try:
                    with open(cache, errors="replace") as f:
                        for line in f:
                            if line.startswith("CMAKE_HOME_DIRECTORY:"):
                                src = line.split("=", 1)[1].strip()
                                dh = os.path.join(src, "scripts", "dev_heavy.sh")
                                if os.path.isfile(dh):
                                    return dh
                except OSError:
                    pass
                break
            parent = os.path.dirname(p)
            if parent == p:
                break
            p = parent
        return "scripts/dev_heavy.sh"


# ------------------------------------------------------------- prose / briefs --

NEGATION = re.compile(r"\b(never|don'?t|do not|not|no|forbidden|prohibited|avoid|instead|"
                      r"bypass(es|ed)?|wrong|incident|withdrawn|unlimited|unbounded|against|refused|refuses|denied|blocked)\b|❌|✗",
                      re.I)
# Command-shaped mentions only: `pkill <arg>` in backticks, at a line start or with an
# option; not prose like "a pkill-by-name" or "name-scoped pkill is machine-wide".
PROSE_PKILL = re.compile(r"(?<![\w/.-])(pkill|killall)\s+((?:-[\w=-]+\s+)*)['\"]?[\w/^.$]")
PROSE_LINE_START = re.compile(r"^\s*(?:[-*>]|\d+[.)])?\s*(?:\$\s*)?$")
PROSE_RULES = [
    (re.compile(r"\bcmake\s+--build\b"), "`cmake --build` outside `scripts/dev_heavy.sh build`",
     "build"),
    (re.compile(r"(?<![\w/.-])(g?make|ninja(-build)?)\s+(\S+\s+)*?-j"), "a `make`/`ninja` with `-j`",
     "build"),
    (re.compile(r"\bBEZEL_JOBSERVER=(?!on\b)\S"), "`BEZEL_JOBSERVER=off`", "build"),
    (re.compile(r"\b(MAKEFLAGS|MFLAGS)="), "a hand-set MAKEFLAGS", "build"),
    (PROSE_PKILL, "a by-name `pkill`/`killall`", "kill"),
    (re.compile(r"\bxargs\s+(-\S+\s+)*kill\b"), "`xargs kill`", "kill"),
    (re.compile(r"\bkill\s+(-\S+\s+)*['\"]?(\$\(|`)\s*(pgrep|pidof)\b"), "`kill $(pgrep ...)`",
     "kill"),
]
# A pkill line that is scoped by PID or by a lane's own path is not a by-name kill.
PROSE_KILL_SCOPED = re.compile(r"readlink -f|realpath|/worktrees/[^/\s]+/|/lane-builds/[^/\s]+/|"
                               r"\s-(P|F|s|g)\s|--parent|--pidfile")


def lint_prose(text: str) -> list[tuple[str, str]]:
    """(kind, finding) for each line that instructs a forbidden build or kill."""
    bad: list[tuple[str, str]] = []
    for line in text.splitlines():
        if NEGATION.search(line):
            continue
        for rx, what, kind in PROSE_RULES:
            m = rx.search(line)
            if not m:
                continue
            if kind == "build" and ("dev_heavy.sh build" in line[:m.start()] or
                                    "agent_check.sh" in line[:m.start()]):
                continue
            # A pkill's scope follows it; an `xargs kill`'s or `kill $(...)`'s source
            # (`pgrep -f <lane path>/ | xargs kill`) may precede it.
            if kind == "kill" and PROSE_KILL_SCOPED.search(
                    line[m.start():] if rx is PROSE_PKILL else line):
                continue
            if rx is PROSE_PKILL and not (line[:m.start()].count("`") % 2 or
                                          PROSE_LINE_START.match(line[:m.start()]) or
                                          m.group(2)):
                continue  # an English mention ("a pkill is machine-wide"), not a command
            bad.append((kind, f"{what}: {line.strip()[:160]}"))
            break
    return bad


# ------------------------------------------------------------------ plumbing --

def under_roots(path: str, roots: list[str]) -> bool:
    if not path:
        return False
    try:
        rp = os.path.realpath(path)
    except OSError:
        rp = path
    for r in roots:
        rr = os.path.realpath(r)
        if rp == rr or rp.startswith(rr.rstrip("/") + "/") or \
                path == r or path.startswith(r.rstrip("/") + "/"):
            return True
    return False


def decide(event: dict, roots: list[str]) -> str | None:
    """The denial reason for this tool call, or None to let it through."""
    tool = event.get("tool_name", "")
    ti = event.get("tool_input") or {}
    cwd = event.get("cwd") or os.getcwd()
    in_scope = under_roots(cwd, roots)
    if tool == "Bash":
        cmd = ti.get("command") or ""
        if not in_scope and not any(r in cmd for r in roots):
            return None
        g = Guard(cwd, roots)
        g.check_script(cmd)
        parts = []
        if g.reasons:
            parts.append("build_guard: refused, this build would bypass the machine-wide build "
                         "jobserver.\n- " + "\n- ".join(g.reasons) +
                         "\nWhy: scripts/build_token.sh header (2026-09-30 OOM). "
                         "Builds go through scripts/agent_check.sh or `scripts/dev_heavy.sh "
                         "build <cmake|make|ninja> ...` with the build tool right after `build`.")
        if g.kill_reasons:
            parts.append(KILL_REFUSAL + "\n- " + "\n- ".join(g.kill_reasons))
        return "\n\n".join(parts) or None
    if not in_scope:
        return None
    if tool in ("Agent", "Task", "SendMessage"):
        text = "\n".join(str(ti.get(k) or "") for k in ("prompt", "message", "content"))
    elif tool in ("Write", "Edit"):
        path = ti.get("file_path") or ""
        if not re.search(r"brief", os.path.basename(path), re.I):
            return None
        text = ti.get("content") or ti.get("new_string") or ""
    else:
        return None
    bad = lint_prose(text)
    if not bad:
        return None
    kills = [b for kind, b in bad if kind == "kill"]
    bad = [b for kind, b in bad if kind == "build"]
    if not bad:
        return (KILL_REFUSAL + " This text tells an agent to run such a kill.\n- " +
                "\n- ".join(kills) + "\nWrite the step as: " + KILL_INSTEAD +
                ". A line that forbids it (\"never ...\") is fine as it is.")
    return ("build_guard: refused, this text tells an agent to build outside the "
            "machine-wide jobserver (the 2026-09-30 OOM started from exactly such a brief "
            "step).\n- " + "\n- ".join(bad) +
            "\nWrite the step as `scripts/agent_check.sh --area <group> --build-only` "
            "(tests) or `scripts/dev_heavy.sh build cmake --build build --target <t>`, "
            "with no `-j`. A line that forbids the command (\"never ...\") is fine as it is.")


def decide_safely(event, roots: list[str]) -> str | None:
    """decide(), except that any internal error lets the call through: a broken
    hook must never stop a session, and build_token.sh still bounds the build."""
    try:
        if not isinstance(event, dict):
            return None
        return decide(event, roots)
    except Exception as e:
        print(f"build_guard: internal error, not checked: {e!r}", file=sys.stderr)
        return None


def _log(path: str, event: dict, reason: str):
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        ti = event.get("tool_input") or {}
        what = ti.get("command") or ti.get("file_path") or ti.get("description") or ""
        with open(path, "a") as f:
            f.write(json.dumps({
                "ts": time.strftime("%Y-%m-%dT%H:%M:%S"),
                "session": event.get("session_id"), "agent": event.get("agent_id"),
                "tool": event.get("tool_name"), "cwd": event.get("cwd"),
                "input": what[:400], "reason": reason[:400]}) + "\n")
    except OSError:
        pass


def main(argv: list[str]) -> int:
    roots: list[str] = []
    deny_log = DENY_LOG
    i = 0
    while i < len(argv):
        if argv[i] == "--root" and i + 1 < len(argv):
            roots.append(argv[i + 1].rstrip("/") or "/")
            i += 2
        elif argv[i] == "--deny-log" and i + 1 < len(argv):
            deny_log = argv[i + 1]
            i += 2
        else:
            i += 1
    try:
        event = json.load(sys.stdin)
    except Exception:
        return 0
    reason = decide_safely(event, roots)
    if reason:
        _log(deny_log, event, reason)
        json.dump({"hookSpecificOutput": {
            "hookEventName": "PreToolUse",
            "permissionDecision": "deny",
            "permissionDecisionReason": reason}}, sys.stdout)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
