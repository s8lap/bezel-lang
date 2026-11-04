#!/usr/bin/env python3
"""reap_waiter_target.py: decide whether a shell WAITER loop's target is satisfied.

Used by scripts/reap_stale.sh for its tier W ("waiter-target-satisfied").

WHY (2026-11-05). A waiter such as

    until [ -s OUT ] && ! pgrep -f ratchet_sweep >/dev/null; do sleep 10; done

can never exit: the shell running it has the literal text `ratchet_sweep` in
its own argv, so its `pgrep -f ratchet_sweep` always finds ITSELF. Age alone
cannot tell that waiter from a live one waiting for a slow job, and a young one
is missed entirely. A waiter like that is stale when (a) the loop condition,
evaluated with the GENUINE target, says EXIT, and (b) the waiter ITSELF matches
its own pattern, so the condition as it sees it says CONTINUE forever. That is
a decided case: the waiter is provably blind.

The GENUINE target is every process matching the pattern EXCEPT the waiter
itself and its own direct `pgrep`/`grep` children (its poll in flight). Nothing
else is discounted (r39-rev-reaper B2, 2026-11-05): not its other descendants
(a backgrounded child may BE the target: `( exec -a X sleep N ) & while pgrep
-f X`), not its ancestors (a script may poll for itself from a background
loop), and not other processes whose argv merely mentions `pgrep` and the
pattern (a live target may). Any such match makes the verdict WAITING, which
reap_stale.sh treats as EXEMPT. A process that appears between the /proc
snapshot and the pgrep is re-read live; if it cannot be read it is a match.

What is deliberately NOT decided. A waiter whose target is satisfied and that
is NOT blind (`until grep -q DONE log` with DONE already in the log) should exit
at its next iteration by itself; if it does not, the likeliest explanation is
that this parser disagrees with bash, and a false "stale" here is a killed live
waiter. Such a loop is reported UNDECIDABLE and left to the age tiers. So is
anything the parser does not fully understand: an unresolved `$VAR`, a command
substitution, a glob, a pipe, a nested loop, more than one loop in the command,
a redirection to a real file, an unknown command or flag.

Usage:  reap_waiter_target.py PID...
Output: one line per pid:  PID<TAB>VERDICT<TAB>DETAIL
        VERDICT is SATISFIED, WAITING or UNDECIDABLE.
"""
import os
import re
import subprocess
import sys

SHELLS = {"bash", "sh", "dash", "zsh", "ksh"}
LOOP_CHILD_COMMS = {"sleep", "pgrep", "grep", "test", "["}
NAME_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*$")
ASSIGN_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=(.*)$", re.S)
REDIR_OPS = {">", ">>", ">|", "<", ">&", "<&", "&>", "&>>"}
CMD_SEPS = {";", "&&", "||", "|", "&", "(", ")", "|&"}


class Undecidable(Exception):
    pass


# ---------------------------------------------------------------------------
# Tokenizer. A word is a list of parts: ("lit", text, quoted) or
# ("var", name, quoted) or ("unres",). An operator is ("op", text).
# ---------------------------------------------------------------------------
def tokenize(s):
    toks = []
    parts = []
    started = False  # a word is in progress (even an empty quoted one)
    i, n = 0, len(s)

    def flush():
        nonlocal parts, started
        if started:
            toks.append(("word", parts))
        parts, started = [], False

    def lit(text, quoted):
        nonlocal started
        started = True
        if parts and parts[-1][0] == "lit" and parts[-1][2] == quoted:
            parts[-1] = ("lit", parts[-1][1] + text, quoted)
        else:
            parts.append(("lit", text, quoted))

    def dollar(j, quoted):
        """Parse an expansion at s[j] == '$'; return the index after it."""
        nonlocal started
        started = True
        if j + 1 < n and s[j + 1] == "{":
            k = s.find("}", j + 2)
            if k < 0:
                raise Undecidable("unterminated ${")
            name = s[j + 2:k]
            parts.append(("var", name, quoted) if NAME_RE.match(name) else ("unres",))
            return k + 1
        m = re.match(r"[A-Za-z_][A-Za-z0-9_]*", s[j + 1:])
        if m:
            parts.append(("var", m.group(0), quoted))
            return j + 1 + m.end()
        if j + 1 < n and (s[j + 1] in "(`" or s[j + 1].isdigit() or s[j + 1] in "?$!#*@-"):
            parts.append(("unres",))
            # skip a $( ... ) roughly; the word is unresolved either way
            if s[j + 1] == "(":
                depth, k = 0, j + 1
                while k < n:
                    if s[k] == "(":
                        depth += 1
                    elif s[k] == ")":
                        depth -= 1
                        if depth == 0:
                            return k + 1
                    k += 1
                raise Undecidable("unterminated $(")
            return j + 2
        lit("$", quoted)
        return j + 1

    while i < n:
        c = s[i]
        if c in " \t":
            flush(); i += 1; continue
        if c == "\n":
            flush(); toks.append(("op", ";")); i += 1; continue
        if c == "#" and not started:
            k = s.find("\n", i)
            i = n if k < 0 else k
            continue
        if c == "'":
            k = s.find("'", i + 1)
            if k < 0:
                raise Undecidable("unterminated '")
            lit(s[i + 1:k], True); i = k + 1; continue
        if c == '"':
            j = i + 1
            started = True
            while True:
                if j >= n:
                    raise Undecidable('unterminated "')
                d = s[j]
                if d == '"':
                    j += 1; break
                if d == "\\" and j + 1 < n and s[j + 1] in '$`"\\\n':
                    lit(s[j + 1], True); j += 2; continue
                if d == "$":
                    j = dollar(j, True); continue
                if d == "`":
                    raise Undecidable("backquote")
                lit(d, True); j += 1
            i = j; continue
        if c == "\\":
            if i + 1 < n:
                lit(s[i + 1], True)
            i += 2; continue
        if c == "$":
            i = dollar(i, False); continue
        if c == "`":
            raise Undecidable("backquote")
        if c in ";&|<>()":
            # an fd number glued to a redirection: `2>`, `1>&`
            fd = ""
            if c in "<>" and started and len(parts) == 1 and parts[0][0] == "lit" \
                    and not parts[0][2] and parts[0][1].isdigit():
                fd = parts[0][1]
                parts, started = [], False
            flush()
            for op in ("&>>", ";;", "&&", "||", ">>", ">|", ">&", "<&", "&>", "|&", "<<",
                       ";", "&", "|", "<", ">", "(", ")"):
                if s.startswith(op, i):
                    if op in ("<<", ";;"):
                        raise Undecidable("heredoc/case")
                    toks.append(("op", fd + op)); i += len(op); break
            continue
        lit(c, False); i += 1
    flush()
    return toks


def word_text(w, env):
    """The literal value of a word after substituting known assignments, or
    raise Undecidable when it has an unresolved part or would glob/split."""
    out = []
    for p in w:
        if p[0] == "unres":
            raise Undecidable("unresolved expansion")
        if p[0] == "var":
            if env.get(p[1]) is None:  # unknown, or assigned a value we could not resolve
                raise Undecidable("unresolved $" + p[1])
            v = env[p[1]]
            if not p[2] and re.search(r"[\s*?\[]", v):
                raise Undecidable("unquoted $%s would split/glob" % p[1])
            out.append(v)
        else:
            text, quoted = p[1], p[2]
            if not quoted:
                if re.search(r"[*?]", text) or re.search(r"\[.+\]", text) or text.startswith("~"):
                    raise Undecidable("glob/tilde in " + text)
            out.append(text)
    return "".join(out)


def is_word(t, value=None):
    if t[0] != "word":
        return False
    if value is None:
        return True
    w = t[1]
    return len(w) == 1 and w[0][0] == "lit" and w[0][1] == value


def literal(t):
    """The value of a word with no expansions at all, else None."""
    if t[0] != "word" or any(p[0] != "lit" for p in t[1]):
        return None
    return "".join(p[1] for p in t[1])


# ---------------------------------------------------------------------------
# Loop finder.
# ---------------------------------------------------------------------------
def command_starts(toks):
    """Indices of tokens that begin a command."""
    starts, at_start = [], True
    for i, t in enumerate(toks):
        if t[0] == "op":
            at_start = t[1] in CMD_SEPS
            continue
        if at_start:
            starts.append(i)
        at_start = False
        if is_word(t, "do") or is_word(t, "then") or is_word(t, "else") or is_word(t, "{"):
            at_start = True
    return starts


def find_loops(script, env, depth=0):
    """Return a list of (kind, cond_tokens, env_at_loop) for every while/until
    loop found in `script` (recursing into eval payloads)."""
    if depth > 3:
        raise Undecidable("nesting too deep")
    toks = tokenize(script)
    starts = command_starts(toks)
    loops = []
    env = dict(env)
    for idx in starts:
        t = toks[idx]
        lv = literal(t)
        if lv in ("while", "until"):
            # condition: up to `; do`
            j = idx + 1
            cond = []
            while j < len(toks):
                if toks[j] == ("op", ";") and j + 1 < len(toks) and is_word(toks[j + 1], "do"):
                    break
                cond.append(toks[j]); j += 1
            else:
                raise Undecidable("no `; do` after " + lv)
            loops.append((lv, cond, dict(env)))
            continue
        if lv in ("for", "if", "case", "select", "function"):
            # a construct we do not model; only fatal if a loop is in it,
            # which the overall loop count check catches
            continue
        # NAME=value as a whole command (it is followed by a separator)
        nxt = toks[idx + 1] if idx + 1 < len(toks) else ("op", ";")
        if t[0] == "word" and nxt[0] == "op" and nxt[1] in (";", "&&"):
            w = t[1]
            if w and w[0][0] == "lit" and not w[0][2]:
                m = ASSIGN_RE.match(w[0][1])
                if m:
                    rest = [("lit", m.group(2), False)] + list(w[1:]) if m.group(2) else list(w[1:])
                    try:
                        env[m.group(1)] = word_text(rest, env) if rest else ""
                    except Undecidable:
                        env[m.group(1)] = None  # assigned, but to something we cannot know
                    continue
    if not loops:
        for t in toks:
            v = literal(t)
            if v is not None and re.search(r"\b(while|until)\b", v) and v != "while" and v != "until":
                loops.extend(find_loops(v, env, depth + 1))
    return loops


def loop_of(argv):
    """The single (kind, cond, env) loop in a shell's argv, or Undecidable."""
    if not argv or os.path.basename(argv[0]) not in SHELLS:
        raise Undecidable("not a shell")
    script = None
    seen_c = False
    for a in argv[1:]:
        if a == "-c" or (a.startswith("-") and "c" in a[1:] and not a.startswith("--")):
            seen_c = True
            continue
        if a.startswith("-"):
            continue
        if seen_c:
            script = a
        break
    if script is None:
        raise Undecidable("not a `-c` shell")
    loops = find_loops(script, {})
    if len(loops) != 1:
        raise Undecidable("%d loops in the command" % len(loops))
    return loops[0]


# ---------------------------------------------------------------------------
# Condition evaluation.
# ---------------------------------------------------------------------------
def split_atoms(cond):
    """cond tokens -> [(joiner, atom_tokens)], joiner None for the first."""
    out, cur, joiner = [], [], None
    for t in cond:
        if t[0] == "op" and t[1] in ("&&", "||"):
            if not cur:
                raise Undecidable("empty atom")
            out.append((joiner, cur)); cur, joiner = [], t[1]
            continue
        if t[0] == "op" and t[1] in CMD_SEPS:
            raise Undecidable("operator %r in a loop condition" % t[1])
        if is_word(t, "while") or is_word(t, "until") or is_word(t, "do"):
            raise Undecidable("nested loop")
        cur.append(t)
    if not cur:
        raise Undecidable("empty condition")
    out.append((joiner, cur))
    return out


def strip_redirs(atom, env):
    words, i = [], 0
    while i < len(atom):
        t = atom[i]
        if t[0] == "op":
            op = t[1].lstrip("0123456789")
            if op not in REDIR_OPS or i + 1 >= len(atom) or atom[i + 1][0] != "word":
                raise Undecidable("operator %r in an atom" % t[1])
            target = word_text(atom[i + 1][1], env)
            if op in (">&", "<&"):
                if not re.fullmatch(r"\d+|-", target):
                    raise Undecidable("redirect to " + target)
            elif op == "<" or target != "/dev/null":
                raise Undecidable("redirect to " + target)
            i += 2
            continue
        words.append(word_text(t[1], env))
        i += 1
    return words


class Ctx:
    """What the evaluator needs from the live system (overridable in tests)."""

    def __init__(self, pid):
        self.pid = pid
        try:
            self.cwd = os.readlink("/proc/%d/cwd" % pid)
        except OSError:
            raise Undecidable("cannot read the waiter's cwd")
        self.ppid_of = {}
        for d in os.listdir("/proc"):
            if not d.isdigit():
                continue
            try:
                with open("/proc/%s/stat" % d) as f:
                    st = f.read()
                self.ppid_of[int(d)] = int(st[st.rindex(")") + 2:].split()[1])
            except (OSError, ValueError):
                continue

    def is_own_poll(self, p):
        """True iff `p` is a DIRECT child of the waiter whose comm is pgrep or
        grep: the waiter's own poll in flight. Read live, so a child forked
        after the snapshot is recognised; unreadable means "not its poll"."""
        try:
            with open("/proc/%d/stat" % p) as f:
                st = f.read()
            comm = st[st.index("(") + 1:st.rindex(")")]
            ppid = int(st[st.rindex(")") + 2:].split()[1])
        except (OSError, ValueError):
            return False
        return ppid == self.pid and comm in ("pgrep", "grep")

    def children_comms(self):
        out = []
        for c, pp in self.ppid_of.items():
            if pp == self.pid:
                try:
                    with open("/proc/%d/comm" % c) as f:
                        out.append(f.read().strip())
                except OSError:
                    pass
        return out

    def path(self, p):
        return p if os.path.isabs(p) else os.path.join(self.cwd, p)

    def run(self, argv):
        try:
            r = subprocess.run(argv, capture_output=True, text=True, timeout=10)
        except (OSError, subprocess.TimeoutExpired):
            raise Undecidable("could not run " + argv[0])
        return r.returncode, r.stdout

    def pid_alive(self, n):
        try:
            os.kill(n, 0)
            return True
        except ProcessLookupError:
            return False
        except PermissionError:
            return True


def eval_atom(words, ctx):
    """-> (waiter_view_truth, genuine_truth)."""
    neg = False
    while words and words[0] == "!":
        neg, words = not neg, words[1:]
    if not words:
        raise Undecidable("bare !")
    cmd, args = words[0], words[1:]
    if cmd in ("true", ":"):
        v = (True, True)
    elif cmd == "false":
        v = (False, False)
    elif cmd in ("test", "[", "[["):
        if cmd == "[":
            if not args or args[-1] != "]":
                raise Undecidable("[ without ]")
            args = args[:-1]
        if cmd == "[[":
            if not args or args[-1] != "]]":
                raise Undecidable("[[ without ]]")
            args = args[:-1]
        inner_neg = False
        if args and args[0] == "!":
            inner_neg, args = True, args[1:]
        if len(args) != 2 or args[0] not in ("-f", "-e", "-s", "-d", "-r"):
            raise Undecidable("unsupported test " + " ".join(args))
        p = ctx.path(args[1])
        op = args[0]
        if op == "-f":
            r = os.path.isfile(p)
        elif op == "-d":
            r = os.path.isdir(p)
        elif op == "-r":
            r = os.access(p, os.R_OK)
        elif op == "-s":
            try:
                r = os.path.getsize(p) > 0
            except OSError:
                r = False
        else:
            r = os.path.exists(p)
        r = r != inner_neg
        v = (r, r)
    elif cmd == "grep":
        flags, pattern, files, i, opts_done = [], None, [], 0, False
        while i < len(args):
            a = args[i]
            if not opts_done and a == "--":
                opts_done = True
            elif not opts_done and a == "-e" and i + 1 < len(args) and pattern is None:
                pattern = args[i + 1]; i += 1
            elif not opts_done and a in ("--quiet", "--silent", "--no-messages"):
                pass
            elif not opts_done and a in ("--fixed-strings", "--extended-regexp", "--ignore-case",
                                         "--word-regexp", "--line-regexp"):
                flags.append(a)
            elif not opts_done and a.startswith("-") and len(a) > 1:
                if not set(a[1:]) <= set("qsFEGxwih"):
                    raise Undecidable("grep flag " + a)
                flags += ["-" + ch for ch in a[1:] if ch not in "qs"]
            elif pattern is None:
                pattern = a
            else:
                files.append(a)
            i += 1
        if pattern is None or len(files) != 1:
            raise Undecidable("grep needs one pattern and one file")
        rc, _ = ctx.run(["grep", "-q", "-s"] + flags + ["-e", pattern, "--", ctx.path(files[0])])
        v = (rc == 0, rc == 0)
    elif cmd == "pgrep":
        flags, pattern = [], None
        for a in args:
            if a == "--":
                continue
            if a.startswith("-") and pattern is None:
                if not set(a[1:]) <= set("fx"):
                    raise Undecidable("pgrep flag " + a)
                flags.append(a)
            elif pattern is None:
                pattern = a
            else:
                raise Undecidable("pgrep with two patterns")
        if pattern is None:
            raise Undecidable("pgrep without a pattern")
        rc, out = ctx.run(["pgrep"] + flags + ["--", pattern])
        if rc not in (0, 1):
            raise Undecidable("pgrep failed")
        raw = {int(x) for x in out.split() if x.isdigit()} - {os.getpid()}
        # Discount ONLY the waiter and its own in-flight poll (see the module
        # docstring). Blind = the waiter itself matches, so its pgrep can
        # never come back empty while it lives.
        genuine = {p for p in raw if p != ctx.pid and not ctx.is_own_poll(p)}
        v = (ctx.pid in raw, bool(genuine))
    elif cmd == "kill":
        if len(args) != 2 or args[0] != "-0" or not args[1].isdigit():
            raise Undecidable("unsupported kill")
        r = ctx.pid_alive(int(args[1]))
        v = (r, r)
    else:
        raise Undecidable("unsupported command " + cmd)
    return (v[0] != neg, v[1] != neg)


def eval_cond(atoms, ctx, env, view):
    """Bash && / || (equal precedence, left-assoc, short-circuit). Every atom
    must PARSE even when short-circuited, so an unsupported one is never hidden."""
    parsed = [(j, strip_redirs(a, env)) for j, a in atoms]
    cache = {}

    def val(k):
        if k not in cache:
            cache[k] = eval_atom(list(parsed[k][1]), ctx)
        return cache[k][view]

    result = val(0)
    for k in range(1, len(parsed)):
        j = parsed[k][0]
        if (j == "&&" and result) or (j == "||" and not result):
            result = val(k)
    return result


def decide(argv, ctx):
    kind, cond, env = loop_of(argv)
    atoms = split_atoms(cond)
    seen_w = eval_cond(atoms, ctx, env, 0)
    real = eval_cond(atoms, ctx, env, 1)
    cont_w = seen_w if kind == "while" else not seen_w
    cont_g = real if kind == "while" else not real
    if cont_g:
        return "WAITING", "%s condition not yet satisfied" % kind
    if not cont_w:
        return "UNDECIDABLE", "target satisfied but the waiter is not blind (it should exit by itself)"
    comms = ctx.children_comms()
    if not comms or any(c not in LOOP_CHILD_COMMS for c in comms):
        return "UNDECIDABLE", "target satisfied but the waiter is not inside its loop (children: %s)" % (
            ",".join(comms) or "none")
    return "SATISFIED", "target satisfied, but the waiter's own pgrep matches itself (it can never exit)"


def main(argv):
    for a in argv[1:]:
        if not a.isdigit():
            print("%s\tUNDECIDABLE\tnot a pid" % a)
            continue
        pid = int(a)
        try:
            with open("/proc/%d/cmdline" % pid, "rb") as f:
                args = [x.decode("utf-8", "replace") for x in f.read().split(b"\0") if x]
            verdict, detail = decide(args, Ctx(pid))
        except Undecidable as e:
            verdict, detail = "UNDECIDABLE", str(e)
        except OSError as e:
            verdict, detail = "UNDECIDABLE", "process unreadable (%s)" % e.strerror
        print("%d\t%s\t%s" % (pid, verdict, detail.replace("\t", " ")))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
