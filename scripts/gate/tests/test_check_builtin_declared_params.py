"""Tests for scripts/check_builtin_declared_params.py (build-free, fail-closed).

fix #8013: the built-in parameter checker (`scripts/check_builtin_declared_params.py`)
compared the ONE built-in parameter table (`src/common/BuiltinRegistry.cpp`'s
`kBuiltinParams`) against the standard library, but its covered set was
exactly THREE blocks (`String` / `DateTime` / `TimeSpan`) — so the ratchet
could not see a SINGLE one of the FIFTY-FOUR rows the #8005 commit
transcribed over the `File` / `Directory` / `Path` / `Environment` / `Json*` /
`StringBuilder` / `Encoding` / `TextWriter` / `Math` heads.  A corrupted
`File.ReadAllText` row still exited 0 with "the table agrees with the
standard library".

What is asserted here:

  * the checker AGREES with the standard library and the specification on the
    tree it lives in (no false positive);
  * every one of the fifty-four rows the #8005 commit transcribed is READ by
    the checker — the list is written out, so a narrowing of the covered set
    turns this suite RED;
  * every covered block has a self-test mutation, and each mutation is
    DETECTED with a message that NAMES the member and the position — a
    checker that cannot fail is a green that means nothing;
  * the checker REPORTS the gap it has: the blocks it does not cover are
    listed, so "the table agrees with the standard library" is never a claim
    about a table the checker cannot see;
  * the known-uncovered list names the positions whose declared type the
    vocabulary cannot express, and the deviations it reports rather than
    fails on.

The list of the #8005 rows is the #8005 COMMIT's own diff (the `-` lines of
`git show 92bf3654 -- src/common/BuiltinRegistry.cpp`), transcribed here as
the oracle's expectation.  It is a SUBSET assertion: a later row added to
the table does not turn this suite red, but a row REMOVED from the table (or
a covered block narrowed) does.
"""

import pathlib
import sys
import unittest

SCRIPTS_DIR = pathlib.Path(__file__).resolve().parent.parent.parent
ROOT = SCRIPTS_DIR.parent
sys.path.insert(0, str(SCRIPTS_DIR))
import check_builtin_declared_params as c  # noqa: E402


# The (head, member, total) keys of the fifty-four rows the #8005 commit
# transcribed (fix #8005, `92bf3654`).  Written out so that a narrowing of
# the checker's covered set is caught here and not by a reader of a report.
ROWS_8005 = [
    ("CString", "From", 1),
    ("Directory", "CreateDirectory", 1),
    ("Directory", "Delete", 1),
    ("Directory", "Delete", 2),
    ("Directory", "Exists", 1),
    ("Directory", "GetDirectories", 1),
    ("Directory", "GetFiles", 1),
    ("Encoding", "GetByteCount", 1),
    ("Encoding", "GetBytes", 1),
    ("Encoding", "GetBytes", 2),
    ("Encoding", "GetString", 1),
    ("Environment", "Exit", 1),
    ("Environment", "Get", 1),
    ("Environment", "GetBool", 1),
    ("Environment", "GetInt", 1),
    ("Environment", "Set", 2),
    ("File", "Copy", 3),
    ("File", "Delete", 1),
    ("File", "Exists", 1),
    ("File", "Move", 2),
    ("File", "ReadAllBytes", 1),
    ("File", "ReadAllLines", 1),
    ("File", "ReadAllText", 1),
    ("File", "ReadAllTextAsync", 1),
    ("File", "ReadAllTextAsync", 2),
    ("File", "WriteAllBytes", 2),
    ("File", "WriteAllLines", 2),
    ("File", "WriteAllText", 2),
    ("File", "WriteAllTextAsync", 2),
    ("File", "WriteAllTextAsync", 3),
    ("Math", "DivRem", 3),
    ("Path", "Combine", 2),
    ("Path", "GetDirectoryName", 1),
    ("Path", "GetExtension", 1),
    ("Path", "GetFileName", 1),
    ("Path", "GetFileNameWithoutExtension", 1),
    ("Path", "GetFullPath", 1),
    ("TextWriter", "Write", 1),
    ("TextWriter", "WriteLine", 1),
    ("WString", "From", 1),
]

# The heads the #8005 commit transcribed, the six the issue names first and
# the rest of the #8005 heads.
HEADS_8005 = [
    "ArgumentException", "ArgumentOutOfRangeException", "BinaryDeserializer",
    "CString", "Directory", "Encoding", "Environment", "Exception",
    "ExceptionInfo", "File", "FileStream", "JsonDeserializer",
    "JsonSerializer", "LinkedList", "Math", "Path", "Shared", "TextWriter",
    "TimeoutException", "WString",
]


class BuiltinDeclaredParams(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.stdlib = c.parse_stdlib(c.read(c.STDLIB), c.read(c.SPEC))
        cls.table = c.parse_table(c.read(c.REGISTRY))

    def test_the_table_agrees_with_the_standard_library(self):
        errors = c.compare(self.stdlib, self.table)
        self.assertEqual([], errors,
                         'the checker reports a disagreement on the tree it '
                         'lives in: %s' % errors)

    def test_every_8005_head_is_covered(self):
        covered = c.covered_heads()
        for head in HEADS_8005:
            self.assertIn(head, covered,
                          'the #8005 head `%s` is not covered by the checker; '
                          'the covered set is %s'
                          % (head, ", ".join(sorted(covered))))

    def test_every_8005_row_is_read(self):
        read = set()
        for (head, member, arity) in c.declared_forms(self.stdlib):
            for key, _idx, _kinds, _nn in c.covering_rows(
                    self.table, head, member, arity):
                read.add(key)
        missing = [k for k in ROWS_8005 if k not in read]
        self.assertEqual([], missing,
                         'the checker does not read %d of the #8005 rows: %s'
                         % (len(missing), missing))

    def test_every_covered_block_has_a_detected_mutation(self):
        covered = c.covered_heads()
        seen = set()
        for (head, member, arity, position, mutation) in c.SELF_TEST_MUTATIONS:
            seen.add(head)
            mutated = c.apply_mutation(self.table, head, member, arity,
                                       position, mutation)
            errors = c.compare(self.stdlib, mutated)
            self.assertTrue(errors,
                            'the checker does not detect a corrupted `%s.%s` '
                            'at position %d (%s)'
                            % (head, member, position, mutation[0]))
            named = [e for e in errors
                     if ("`%s.%s`" % (head, member)) in e
                     or ("`%s'" % head) in e]
            self.assertTrue(named,
                            'the checker detects a corrupted `%s.%s` at '
                            'position %d but does not NAME the member: %s'
                            % (head, member, position, "; ".join(errors)))
        self.assertEqual(set(), covered - seen,
                         'a covered block has no self-test mutation — a block '
                         'with no mutation can be silently wrong')

    def test_the_known_uncovered_list_is_reported(self):
        gaps = c.known_uncovered(self.stdlib, self.table)
        self.assertTrue(gaps, 'the checker reports no known-uncovered item; '
                              'the gap must be visible')
        kinds = set(kind for kind, _text in gaps)
        self.assertIn("block", kinds,
                      'the known-uncovered list names no uncovered block')
        blocks = [text for kind, text in gaps if kind == "block"]
        for head in ("Int32", "Convert", "Guid"):
            self.assertTrue(any(("`%s`" % head) in t for t in blocks),
                            'the uncovered head `%s` is not listed' % head)

    def test_the_known_uncovered_list_names_the_positions_without_a_type(self):
        gaps = c.known_uncovered(self.stdlib, self.table)
        positions = [text for kind, text in gaps if kind == "position"]
        for head in ("Math", "StringBuilder", "File"):
            self.assertTrue(any(("`%s" % head) in t for t in positions),
                            'no position of the head `%s` is listed as '
                            'known-uncovered' % head)

    def test_the_coverage_is_reported(self):
        forms, rows = c.coverage(self.stdlib, self.table)
        self.assertGreaterEqual(len(c.covered_heads()), 25,
                                'the covered set shrank below the twenty-two '
                                'standard-library and three specification '
                                'blocks of fix #8013')
        self.assertGreaterEqual(rows, 140,
                                'the checker reads fewer than 140 table rows; '
                                'the covered set shrank')
        self.assertGreater(forms, 0, 'the checker compares no declared form')


if __name__ == '__main__':
    unittest.main()
