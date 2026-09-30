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


def _base(word: str) -> str:
    return os.path.basename(word.rstrip("/"))


# ---------------------------------------------------------------- tokenizer --

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
    def __init__(self, cwd: str):
        self.cwd = cwd
        self.vcwd = cwd
        self.reasons: list[str] = []

    def deny(self, why: str, instead: str | None = None):
        msg = why if not instead else f"{why}\n  Run instead: {instead}"
        if msg not in self.reasons:
            self.reasons.append(msg)

    # -- entry --------------------------------------------------------------
    def check_script(self, text: str, depth: int = 0):
        if depth > MAX_DEPTH or not text:
            return
        text, shell_bodies = _split_heredocs(text)
        cmds, nested = tokenize(text)
        for words in cmds:
            self.check_words(words, depth)
        for sub in nested + shell_bodies:
            self.check_script(sub, depth + 1)

    # -- one simple command --------------------------------------------------
    def check_words(self, words: list[str], depth: int):
        for w in words:
            self.check_tamper(w)
        c = self.command_index(words, 0)
        if c is None:
            return
        name = _base(words[c])
        args = words[c + 1:]
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
        probe = Guard(self.cwd)
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
                      r"bypass(es|ed)?|wrong|incident|withdrawn|unlimited|unbounded)\b|❌|✗",
                      re.I)
PROSE_RULES = [
    (re.compile(r"\bcmake\s+--build\b"), "`cmake --build` outside `scripts/dev_heavy.sh build`"),
    (re.compile(r"(?<![\w/.-])(g?make|ninja(-build)?)\s+(\S+\s+)*?-j"), "a `make`/`ninja` with `-j`"),
    (re.compile(r"\bBEZEL_JOBSERVER=(?!on\b)\S"), "`BEZEL_JOBSERVER=off`"),
    (re.compile(r"\b(MAKEFLAGS|MFLAGS)="), "a hand-set MAKEFLAGS"),
]


def lint_prose(text: str) -> list[str]:
    bad: list[str] = []
    for line in text.splitlines():
        if NEGATION.search(line):
            continue
        for rx, what in PROSE_RULES:
            m = rx.search(line)
            if not m:
                continue
            if "dev_heavy.sh build" in line[:m.start()] or "agent_check.sh" in line[:m.start()]:
                continue
            bad.append(f"{what}: {line.strip()[:160]}")
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
        g = Guard(cwd)
        g.check_script(cmd)
        if not g.reasons:
            return None
        return ("build_guard: refused, this build would bypass the machine-wide build "
                "jobserver.\n- " + "\n- ".join(g.reasons) +
                "\nWhy: scripts/build_token.sh header (2026-09-30 OOM). "
                "Builds go through scripts/agent_check.sh or `scripts/dev_heavy.sh build "
                "<cmake|make|ninja> ...` with the build tool right after `build`.")
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
