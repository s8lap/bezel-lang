"""#3300 — a `#` comment must never sit inside a `\\`-continued command.

Bash ends a command at a `#` comment even in the middle of a backslash
continuation: the comment runs to the newline, and every remaining continued
line becomes a separate command. There is no warning at parse or run time.

`run_parallel.sh` built the `gate.summary format-failure` call across a
continuation with a seven-line explanatory comment in the middle of it, so
`--timed-out` and the REQUIRED `--logdir` were never passed. argparse then
exited 2 on a usage error for EVERY run, `_rp_py_failure` was always empty, and
the #1861 GATE INTEGRITY banner — the one that names which shard crashed and
which test it died on — has never printed in the life of the script. The usage
error was visible in every gate log; nobody read it as a defect.

The run's exit path is independent (run_parallel.sh's final `if` still fails on
a crashed or undercounting shard), so this cost diagnostics, not soundness. But
it means the `--timed-out` plumbing added by the narrow #3222 work never reached
summary.py at all.

A comment cannot enforce this rule — the comment that would have prevented it is
the one that caused it, by being in the wrong place. A test can.
"""
import pathlib
import re
import subprocess
import unittest

REPO = pathlib.Path(__file__).resolve().parents[3]
SCRIPT = REPO / 'tests' / 'run_parallel.sh'


def logical_lines(text):
    """Yield (start_lineno, joined_text) for each `\\`-continued logical line."""
    raw = text.split('\n')
    i = 0
    while i < len(raw):
        start, buf = i, [raw[i]]
        while buf[-1].rstrip().endswith('\\') and i + 1 < len(raw):
            i += 1
            buf.append(raw[i])
        yield start + 1, buf
        i += 1


class ContinuationCommentTest(unittest.TestCase):
    """No continued command may contain a comment line after its first line."""

    def test_no_comment_inside_a_backslash_continuation(self):
        text = SCRIPT.read_text()
        offenders = []
        for lineno, buf in logical_lines(text):
            if len(buf) == 1:
                continue
            # The first line may legitimately be a comment (a run of comment
            # lines each ending in `\` is not a command). Only a comment
            # appearing AFTER a non-comment first line amputates a command.
            if buf[0].lstrip().startswith('#'):
                continue
            for off, cont in enumerate(buf[1:], start=1):
                if cont.lstrip().startswith('#'):
                    offenders.append((lineno + off, cont.strip()[:70]))
        self.assertEqual(
            offenders, [],
            "a `#` comment inside a `\\`-continuation silently truncates the "
            "command and turns the remaining lines into separate commands "
            "(#3300); move the prose above the command:\n" +
            '\n'.join(f"  {SCRIPT.name}:{n}: {t}" for n, t in offenders))


class FormatFailureArgvTest(unittest.TestCase):
    """The specific call #3300 amputated must carry every REQUIRED flag."""

    # Sourced from scripts/gate/summary.py's format-failure parser.
    # sharpen #3668: --missing-summary joined the required set. The
    # completed-shard suppression in format_failure_banner keys on ABSENCE
    # from that list, so a defaulted empty list is a fail-open default: a
    # caller that forgets the flag relabels every crashed shard "completed
    # its slice" and drops its `died on:` suspect.
    REQUIRED = ('--ran', '--expected', '--nbin', '--njobs', '--logdir',
                '--missing-summary')

    def _invocation(self):
        text = SCRIPT.read_text()
        for lineno, buf in logical_lines(text):
            joined = ' '.join(l.rstrip().rstrip('\\') for l in buf)
            if 'format-failure' in joined and not buf[0].lstrip().startswith('#'):
                return joined
        self.fail("no format-failure invocation found in run_parallel.sh")

    def test_every_required_flag_reaches_the_command(self):
        joined = self._invocation()
        for flag in self.REQUIRED:
            self.assertIn(
                flag, joined,
                f"{flag} is REQUIRED by summary.py's format-failure parser but "
                f"never reaches it — the call exits 2 and the #1861 gate "
                f"integrity banner silently does not print (#3300)")

    def test_summary_py_still_requires_exactly_these(self):
        """If summary.py grows a required flag, this test must be updated too."""
        src = (REPO / 'scripts' / 'gate' / 'summary.py').read_text()
        block = src[src.index('prog="summary.py format-failure"'):]
        block = block[:block.index('def ', 200)] if 'def ' in block[200:] else block
        found = set(re.findall(
            r'add_argument\("(--[a-z-]+)"[^)]*required=True', block, re.S))
        self.assertEqual(
            found, set(self.REQUIRED),
            "summary.py's REQUIRED format-failure flags changed; update "
            "FormatFailureArgvTest.REQUIRED and the run_parallel.sh call")


class EmptyArrayDefaultTest(unittest.TestCase):
    """`"${arr[@]:-}"` on an EMPTY array yields ONE EMPTY STRING, not zero args.

    summary.py's `has_defect = bool(... or timed_out_shards)` is truthy for
    [""], so a phantom element makes every CLEAN run print a GATE FAILURE
    banner claiming a shard hit the per-shard ceiling. That is the shape that
    poisoned 11 retries on d8ab1b7.

    a4c0bdc removed this `:-` at the argument site but left it at the
    declaration; the comment it added there then amputated the whole call
    (#3300), hiding the second instance until #3300 was fixed. Two independent
    bugs, one masking the other.

    An ARRAY ASSIGNMENT is what matters: the two `for _x in "${arr[@]:-}"`
    loops are each guarded by `[[ -n "$_x" ]]`, so a phantom element is
    discarded there.
    """

    def test_no_bare_default_in_an_array_assignment(self):
        offenders = []
        for n, line in enumerate(SCRIPT.read_text().split('\n'), 1):
            stripped = line.strip()
            if stripped.startswith('#'):
                continue
            if re.search(r'^\s*\w+=\(\"\$\{\w+\[@\]:-\}\"\)', line):
                offenders.append((n, stripped[:70]))
        self.assertEqual(
            offenders, [],
            'an array assignment must not use "${arr[@]:-}" — on an empty '
            'array it manufactures a one-element [""] rather than an empty '
            'array (#3300 follow-up):\n' +
            '\n'.join(f"  {SCRIPT.name}:{n}: {t}" for n, t in offenders))

    def test_bash_default_on_empty_array_yields_one_empty_element(self):
        out = subprocess.run(
            ['bash', '-c',
             'declare -a e=(); x=("${e[@]:-}"); echo "${#x[@]}"; '
             'y=("${e[@]}"); echo "${#y[@]}"'],
            capture_output=True, text=True)
        self.assertEqual(out.stdout.split(), ['1', '0'],
                         'bash semantics changed; re-derive the rule above')


class BashBehaviourTest(unittest.TestCase):
    """Pin the bash behaviour itself, so the rule above is not folklore."""

    def test_bash_truncates_a_command_at_an_embedded_comment(self):
        script = 'f() { echo "ARGS:$*"; }\nf --a 1 \\\n  # comment\n  --b 2\n'
        out = subprocess.run(['bash', '-c', script], capture_output=True,
                             text=True)
        self.assertIn('ARGS:--a 1', out.stdout)
        self.assertNotIn('--b', out.stdout)
        self.assertIn('command not found', out.stderr)
