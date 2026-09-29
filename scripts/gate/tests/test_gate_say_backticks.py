"""fix #5055 — backticks in `say` strings execute as command substitution.

A bare backtick inside a double-quoted `say` call is a bash command-substitution
that tries to run the enclosed text as a command.  In gate_full_cycle.sh:1620 and
:1639, the text `` `internal:` `` sat inside a double-quoted `say` argument, so
every gate log showed:

    line 1612: internal:: command not found

Fix: escape each backtick as \\`internal:\\` so it prints literally.

This module drives the real gate_full_cycle.sh (via _code_lines extraction), so
it is registered as "isolated" in test_gate_markers.SelfTestSuiteIsolationTest.KNOWN.
"""

import pathlib
import re
import unittest

HERE = pathlib.Path(__file__).resolve().parent
CYCLE_SH = HERE.parent.parent / "gate_full_cycle.sh"   # scripts/gate_full_cycle.sh


def _code_lines(text):
    """(lineno, line) for every line that is not blank or a full-line comment."""
    out = []
    for n, ln in enumerate(text.splitlines(), 1):
        s = ln.strip()
        if not s or s.startswith("#"):
            continue
        out.append((n, ln))
    return out


# A backtick that is NOT escaped.  Matches a bare ` not preceded by \ .
_UNESCAPED_BACKTICK = re.compile(r'(?<!\\)`')


class SayBacktickTest(unittest.TestCase):
    """Every `say` call in gate_full_cycle.sh must print its backticks literally,
    never execute them as command substitution."""

    def test_no_unescaped_backtick_in_say_calls(self):
        """Scan every `say "..."` call for unescaped backticks.

        A bare ` inside a double-quoted string is a bash command-substitution
        that tries to run the enclosed text as a command.  The correct form is
        \\` (a literal backslash followed by a backtick), which prints as `."""
        text = CYCLE_SH.read_text()
        lines = _code_lines(text)

        # Find all say calls that contain a backtick (escaped or not)
        bad = []
        for n, ln in lines:
            if not (ln.startswith('say ') or ln.startswith('say"')):
                continue
            # Only lines with a backtick need checking
            if '`' not in ln:
                continue
            # Strip the leading "say " or "say" prefix and check inside the string
            suffix = ln.split('say ', 1)[-1] if 'say ' in ln else ln.split('say', 1)[-1]
            # Find unescaped backticks inside the argument
            # Walk through the line looking for ` not preceded by \
            i = 0
            while i < len(suffix):
                if suffix[i] == '`' and (i == 0 or suffix[i-1] != '\\'):
                    bad.append((n, ln, suffix[i:]))
                    break
                i += 1

        self.assertEqual(
            bad, [],
            "gate_full_cycle.sh contains `say` calls with unescaped backticks "
            "that execute as command substitution (#5055):\n"
            + "\n".join(
                "  line %d: %s\n           ^ unescaped backtick here"
                % (n, ln.strip()) for n, ln, _ in bad))


if __name__ == "__main__":
    unittest.main()
