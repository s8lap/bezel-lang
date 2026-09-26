"""Unit tests for scripts/pregate_stale_needles.py (the pre-gate diagnostic-rename scan)."""
import importlib.util
import pathlib
import unittest

_P = pathlib.Path(__file__).resolve().parent.parent.parent / 'pregate_stale_needles.py'
_spec = importlib.util.spec_from_file_location('pregate_stale_needles', _P)
psn = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(psn)


class RemovedLiterals(unittest.TestCase):
    def test_takes_only_removed_lines_and_skips_header(self):
        diff = ('--- a/src/x.cpp "not a removal header literal"\n'
                '-  report("module `M` has no public type here");\n'
                '+  report("module `M` has no exported type here");\n'
                ' context("unchanged message text line");\n')
        self.assertEqual(psn.removed_literals(diff),
                         {'module `M` has no public type here'})

    def test_skips_short_and_spaceless_literals(self):
        diff = '-  f("short one"); g("AnIdentifierLikeLiteralValue");\n'
        self.assertEqual(psn.removed_literals(diff), set())

    def test_handles_escaped_quotes(self):
        diff = '-  r("say \\"hello\\" to the world please");\n'
        self.assertEqual(psn.removed_literals(diff),
                         {'say \\"hello\\" to the world please'})

    def test_min_len_is_respected(self):
        diff = '-  r("four five six");\n'
        self.assertEqual(psn.removed_literals(diff, min_len=10), {'four five six'})
        self.assertEqual(psn.removed_literals(diff, min_len=20), set())

    def test_needs_at_least_three_words(self):
        diff = '-  r("   parameters,   but`   ");\n'
        self.assertEqual(psn.removed_literals(diff, min_len=10), set())


class StillPresent(unittest.TestCase):
    def test_reflowed_message_is_not_gone(self):
        lits = {'ensure its own internal synchronization now'}
        tree = ['r("... ensure its own "\n            "internal synchronization now");']
        self.assertEqual(psn.still_present(lits, tree), lits)

    def test_moved_literal_is_not_gone(self):
        lits = {'moved message text here', 'deleted message text here'}
        self.assertEqual(psn.still_present(lits, ['x("moved message text here")']),
                         {'moved message text here'})


if __name__ == '__main__':
    unittest.main()
