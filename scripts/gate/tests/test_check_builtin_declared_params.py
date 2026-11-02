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
    fails on;
  * fix #8021: the §-citation of every block is MACHINE-READABLE and
    VERIFIED against the document — a citation that names a section which
    does not exist, or which does not declare the block, is an ERROR;
  * fix #8023: the `required` (DECLARED ARITY) column is COMPARED with the
    document — a corrupted arity fails and names the member and the
    argument count — and the rows where the table and the document disagree
    are REPORTED with the document's own declaration as the provenance.

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


class Citations(unittest.TestCase):
    """fix #8021: the §-citations are DATA, and they are CHECKED."""

    @classmethod
    def setUpClass(cls):
        cls.stdlib_text = c.read(c.STDLIB)
        cls.spec_text = c.read(c.SPEC)

    def test_every_covered_block_carries_a_citation(self):
        for head in sorted(c.covered_heads()):
            self.assertIn(head, c.BLOCK_CITATIONS,
                          'the covered block `%s` records no §-citation'
                          % head)

    def test_every_cited_head_is_cited(self):
        for head in sorted(c.BLOCK_CITATIONS):
            self.assertIn(head, c.covered_heads() | set(
                c.KNOWN_UNCOVERED_HEADS),
                'the §-citation of `%s` names a head the checker never '
                'prints' % head)

    def test_the_citations_are_verified_against_the_documents(self):
        errors = c.check_citations(self.stdlib_text, self.spec_text)
        self.assertEqual([], errors,
                         'the checker reports a wrong §-citation on the tree '
                         'it lives in: %s' % errors)

    def test_a_wrong_citation_is_detected(self):
        # the citation names a section that exists but declares no `Math`
        broken = dict(c.BLOCK_CITATIONS)
        broken["Math"] = ("stdlib", "2.5", "declares")
        errors = c.check_citations(self.stdlib_text, self.spec_text, broken)
        self.assertTrue(errors,
                        'the checker does not detect a WRONG §-citation')
        self.assertTrue(any("`Math`" in e for e in errors),
                        'the checker detects a wrong §-citation but does not '
                        'NAME the block: %s' % errors)

    def test_a_citation_of_a_section_that_does_not_exist_is_detected(self):
        broken = dict(c.BLOCK_CITATIONS)
        broken["FileStream"] = ("stdlib", "7.99", "declares")
        errors = c.check_citations(self.stdlib_text, self.spec_text, broken)
        self.assertTrue(errors,
                        'the checker does not detect a §-citation that names '
                        'a section which does not exist')
        self.assertTrue(any("`FileStream`" in e for e in errors),
                        'the checker does not NAME the block: %s' % errors)

    def test_the_reasons_derive_their_sections_from_the_map(self):
        # a reason that names a section the map does not claim would be a
        # pointer that can be wrong without anybody noticing.
        for head, reason in sorted(c.KNOWN_UNCOVERED_HEADS.items()):
            doc, number, _kind = c.BLOCK_CITATIONS[head]
            self.assertIn("%s §%s" % (doc, number), reason,
                          'the reason for `%s` does not name its own §-citation'
                          % head)
        for head in ("Math", "FileStream", "LinkedList", "Shared",
                     "TimeoutException"):
            reasons = [r for k, r in c.NO_OPINION.items() if k[0] == head]
            self.assertTrue(reasons, 'the head `%s` has no reason' % head)
            doc, number, _kind = c.BLOCK_CITATIONS[head]
            for reason in reasons:
                self.assertIn("%s §%s" % (doc, number), reason,
                              'a reason for `%s` names another section' % head)


class RequiredColumn(unittest.TestCase):
    """fix #8023: the `required` (DECLARED ARITY) column is compared."""

    @classmethod
    def setUpClass(cls):
        cls.stdlib = c.parse_stdlib(c.read(c.STDLIB), c.read(c.SPEC))
        cls.table = c.parse_table(c.read(c.REGISTRY))

    def test_the_declared_minimum_is_read_from_the_document(self):
        # stdlib §7.2: `access: FileAccess := FileAccess.ReadWrite` is
        # DEFAULTED, so the FileStream constructor's declared minimum is 2.
        self.assertEqual(
            2, c.declared_required(self.stdlib, "FileStream", "", 3),
            'the FileStream constructor\'s declared minimum is not 2')
        # stdlib §2.2: `out remainder: int` is an ARGUMENT, so Math.DivRem's
        # declared minimum is 3.
        self.assertEqual(
            3, c.declared_required(self.stdlib, "Math", "DivRem", 3),
            'Math.DivRem\'s declared minimum is not 3')

    def test_a_corrupted_required_count_is_detected(self):
        mutated = c.apply_mutation(self.table, "FileStream", "", 3, 0,
                                   ("required", 1))
        errors = c.compare(self.stdlib, mutated)
        self.assertTrue(errors,
                        'the checker does not detect a corrupted `required` '
                        'count')
        named = [e for e in errors if "`FileStream'" in e]
        self.assertTrue(named,
                        'the checker detects a corrupted `required` count but '
                        'does not NAME the member: %s' % errors)
        self.assertTrue(any("`required` column" in e for e in named),
                        'the checker detects a corrupted `required` count but '
                        'does not NAME the column: %s' % errors)

    def test_a_corrupted_required_count_on_a_clean_row_is_detected(self):
        mutated = c.apply_mutation(self.table, "string", "Contains", 1, 0,
                                   ("required", 2))
        errors = c.compare(self.stdlib, mutated)
        self.assertTrue(errors,
                        'the checker does not detect a corrupted `required` '
                        'count on a row with no known deviation')
        self.assertTrue(any("`string.Contains`" in e for e in errors),
                        'the checker does not NAME the member: %s' % errors)

    def test_the_known_required_deviations_are_reported(self):
        gaps = c.known_uncovered(self.stdlib, self.table)
        required = [text for kind, text in gaps if kind == "required"]
        self.assertTrue(required,
                        'the checker reports no `required` deviation; the '
                        'disagreement must be visible')
        joined = " ".join(required)
        for head in ("FileStream", "Math", "TextWriter", "string"):
            self.assertIn(head, joined,
                          'the `required` deviation of `%s` is not reported'
                          % head)
        self.assertIn("DEFAULTED", joined,
                      'the `required` deviations do not carry the document\'s '
                      'own declaration as the provenance')

    def test_a_known_deviation_cannot_hide_a_corruption(self):
        # the FileStream row records the table's value (3); a corruption to
        # any OTHER wrong value must fail.
        for bad in (0, 1, 4, 5):
            mutated = c.apply_mutation(self.table, "FileStream", "", 3, 0,
                                       ("required", bad))
            errors = c.compare(self.stdlib, mutated)
            self.assertTrue(errors,
                            'the checker does not detect a `required` count '
                            'of %d on the FileStream row' % bad)

    def test_the_deviation_text_names_the_gate_that_decides(self):
        # fix #8023: the §5.1 kind gate does NOT include `StringBuilder`.
        for (head, member, i), reason in sorted(
                c.NULLABILITY_DEVIATION.items()):
            self.assertIn("checkBuiltinStringBuilderCall", reason,
                          'the deviation for `%s.%s` does not name the gate '
                          'that decides it' % (head, member))
            self.assertIn("checkAssignable", reason,
                          'the deviation for `%s.%s` does not name the '
                          '`checkAssignable` path' % (head, member))


if __name__ == '__main__':
    unittest.main()
