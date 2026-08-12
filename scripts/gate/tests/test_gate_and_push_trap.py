"""#3158 — gate_and_push.sh must register EXACTLY ONE EXIT trap.

Bash keeps only one EXIT trap: a second `trap … EXIT` silently REPLACES the
first, with no warning at parse or run time. gate_and_push.sh had two — the
donefile/pidfile terminal marker at the top of the file, and the
serialization-lock release registered once the lock is acquired. The later one
won, so on the NORMAL path the terminal marker was never published: no donefile,
and a stale pidfile left behind.

The cost was not a wrong verdict — the push path is untouched — but a silent
one. The waiter machinery added in 383d01c specifically to end the #3010
self-matching-waiter class was dead on arrival, because it was layered onto an
existing EXIT trap (2ea36f2) that nobody noticed. On 2026-08-13 a gate passed
both oracles, pushed, and exited rc=0, and the coordinator waiting on its
donefile learned nothing.

A comment cannot enforce this — the comment that would have prevented it is the
one being added alongside this test. A test can.
"""
import pathlib
import re
import unittest

REPO = pathlib.Path(__file__).resolve().parents[3]
SCRIPT = REPO / 'scripts' / 'gate_and_push.sh'

# A trap REGISTRATION: `trap <handler> ... EXIT`. Matched on a line that is not a
# comment, so the warning comments about this very rule do not count themselves —
# the same false-positive class check_imported_symbol_reads.py hit on its own
# documentation.
TRAP_EXIT = re.compile(r'^\s*trap\s+.*\bEXIT\b')


def _registrations(text):
    return [(i, ln) for i, ln in enumerate(text.splitlines(), 1)
            if TRAP_EXIT.match(ln) and not ln.lstrip().startswith('#')]


class GateAndPushTrapTest(unittest.TestCase):
    def test_exactly_one_exit_trap(self):
        found = _registrations(SCRIPT.read_text())
        self.assertEqual(
            len(found), 1,
            'gate_and_push.sh must register exactly one EXIT trap; a second one '
            'silently replaces the first (#3158). Found %d:\n%s\nAdd the work to '
            '_bezel_gate_signal_done instead of registering another trap.'
            % (len(found), '\n'.join('  line %d: %s' % (n, l.strip())
                                     for n, l in found)))

    def test_the_one_trap_is_the_terminal_marker_handler(self):
        found = _registrations(SCRIPT.read_text())
        self.assertTrue(found, 'no EXIT trap at all — the terminal marker '
                               'would never be published (#3158)')
        self.assertIn(
            '_bezel_gate_signal_done', found[0][1],
            'the surviving EXIT trap must be the handler that writes '
            '$BEZEL_GATE_DONEFILE and removes $BEZEL_GATE_PIDFILE (#3158)')

    def test_handler_captures_status_before_doing_anything(self):
        """`rc=$?` must be the handler's FIRST statement.

        Any command ahead of it clobbers the exit status being reported, so the
        donefile would carry the status of the lock release rather than of the
        gate. This is why the lock release moved INSIDE the handler rather than
        the handler being appended to the lock-release trap.
        """
        text = SCRIPT.read_text()
        body = text.split('_bezel_gate_signal_done() {', 1)[1]
        first = next(l.strip() for l in body.splitlines()
                     if l.strip() and not l.strip().startswith('#'))
        self.assertEqual(first, 'local rc=$?',
                         'the exit status must be captured before anything else '
                         'runs in the handler (#3158); got: %r' % first)

    def test_lock_release_still_happens_on_exit(self):
        """The serialization daemon must still be killed when the script exits.

        Collapsing two traps into one must not drop the job the second one was
        doing — otherwise the flock would outlive the gate and every subsequent
        gate would refuse to start.
        """
        text = SCRIPT.read_text()
        body = text.split('_bezel_gate_signal_done() {', 1)[1].split('\n}', 1)[0]
        self.assertIn('_gate_serialize_pid', body,
                      'the single EXIT handler must also release the '
                      'serialization lock (#3158)')
        self.assertIn('kill "$_gate_serialize_pid"', body)


if __name__ == '__main__':
    unittest.main()
