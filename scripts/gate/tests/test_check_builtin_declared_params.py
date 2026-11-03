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
    argument count;
  * fix #8034: the EIGHT rows where the table and the document disagreed are
    CORRECTED in the table itself, so the checker's registry of known
    deviations is gone: there is NO `required` deviation left to REPORT, and
    a `required` that is not the document's declared minimum now FAILS the
    checker.

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


class ParameterNames(unittest.TestCase):
    """fix #7828: the `names` column — the §22.5 NAME SURFACE — is COMPARED.

    stdlib §1 says the parameter names are API surface ("parameter names are
    API surface (named arguments, spec §22.5)"), and spec §22.5 binds a
    named argument to the parameter it NAMES — so a name the document does
    not declare is a wrong §22.5 binding, and a name the document DOES
    declare and the table does not is a refused binding.

    The checker compared the parameter TYPE, the §7.3 NULLABILITY and the
    `required` columns, and NEVER the `names` column: a row could carry
    `pth` where the document spells `path` — the very defect #7828 reports
    for the `File` / `Directory` rows — and the checker still printed "the
    table agrees with the standard library".
    """

    @classmethod
    def setUpClass(cls):
        cls.stdlib = c.parse_stdlib(c.read(c.STDLIB), c.read(c.SPEC))
        cls.table = c.parse_table(c.read(c.REGISTRY))

    def test_the_declared_names_are_read_from_the_document(self):
        # stdlib §7.3: `File.Exists(path: string)`.
        self.assertEqual(
            {("path",)},
            c.declared_names(self.stdlib, "File", "Exists", 1),
            'stdlib §7.3\'s `File.Exists` names are not read from the document')
        # stdlib §7.4: `Directory.Delete(path: string, recursive: bool :=
        # false)` — the defaulted second parameter's name is declared too.
        self.assertEqual(
            {("path", "recursive")},
            c.declared_names(self.stdlib, "Directory", "Delete", 2),
            'stdlib §7.4\'s `Directory.Delete` names are not read from the '
            'document')

    def test_a_positional_only_slot_declares_no_name(self):
        # stdlib §2.5: `TryParse(s: string, out result: DateTime): bool` — an
        # `out` slot is positional-only (spec §22.5), so it declares no §22.5
        # name and the table's empty name there is correct.
        names = c.declared_names(self.stdlib, "DateTime", "TryParse", 2)
        self.assertIn(("s", ""), names,
                      'the `out` slot does not declare a §22.5 name: %s'
                      % (names,))

    def test_a_corrupted_parameter_name_is_detected(self):
        # the §22.5 NAME the document does not declare: `pth` where stdlib
        # §7.3 declares `path`.
        mutated = c.apply_mutation(self.table, "File", "Exists", 1, 0,
                                   ("name", "pth"))
        errors = c.compare(self.stdlib, mutated)
        self.assertTrue(errors,
                        'the checker does not detect a corrupted §22.5 NAME')
        named = [e for e in errors if "`File.Exists`" in e]
        self.assertTrue(named,
                        'the checker detects a corrupted §22.5 NAME but does '
                        'not NAME the member: %s' % errors)
        self.assertTrue(any("`names` column" in e for e in named),
                        'the checker detects a corrupted §22.5 NAME but does '
                        'not NAME the column: %s' % errors)

    def test_a_corrupted_directory_name_is_detected(self):
        # stdlib §7.4's `Directory.Delete(path: string, recursive: bool :=
        # false)`: the SECOND name is corrupted here, so the comparison is
        # not a one-position accident.
        mutated = c.apply_mutation(self.table, "Directory", "Delete", 2, 1,
                                   ("name", "recurse"))
        errors = c.compare(self.stdlib, mutated)
        self.assertTrue(errors,
                        'the checker does not detect a corrupted §22.5 NAME '
                        'on the `Directory.Delete` row')
        named = [e for e in errors if "`Directory.Delete`" in e]
        self.assertTrue(named,
                        'the checker detects a corrupted §22.5 NAME but does '
                        'not NAME the member: %s' % errors)
        self.assertTrue(any("`names` column" in e for e in named),
                        'the checker detects a corrupted §22.5 NAME but does '
                        'not NAME the column: %s' % errors)

    def test_a_removed_name_surface_is_detected(self):
        # fix #7828: the defect the issue REPORTS — the rows carry NO
        # parameter names, so a named argument binds by position.  A checker
        # that only compared a name it FOUND would miss it, so the removed
        # name surface must be detected too.
        mutated = c.apply_mutation(self.table, "File", "Exists", 1, 0,
                                   ("names",))
        errors = c.compare(self.stdlib, mutated)
        self.assertTrue(errors,
                        'the checker does not detect a REMOVED §22.5 NAME '
                        'SURFACE')
        named = [e for e in errors if "`File.Exists`" in e]
        self.assertTrue(named,
                        'the checker detects a removed §22.5 name surface but '
                        'does not NAME the member: %s' % errors)
        self.assertTrue(any("`names` column" in e for e in named),
                        'the checker detects a removed §22.5 name surface but '
                        'does not NAME the column: %s' % errors)

    def test_the_names_of_the_covered_rows_match_the_document(self):
        # no false positive: the table's covered rows carry the DOCUMENT's
        # own names, so the comparison is clean on the tree it lives in.
        errors = c.compare(self.stdlib, self.table)
        self.assertEqual([], errors,
                         'the checker reports a disagreement on the tree it '
                         'lives in: %s' % errors)


class RequiredColumn(unittest.TestCase):
    """fix #8023: the `required` (DECLARED ARITY) column is compared.

    fix #8034: the EIGHT rows whose `required` contradicted the document are
    CORRECTED in the table, so a `required` disagreement is a FAILURE and
    not a report — this class pins that stronger oracle.
    """

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

    def test_no_required_deviation_remains_and_a_corrupted_required_fails(self):
        # fix #8034: the EIGHT rows whose `required` contradicted the
        # document are CORRECTED in the table itself, so the checker's
        # registry of known `required` deviations is gone and there is NO
        # `required` gap left to REPORT.
        gaps = c.known_uncovered(self.stdlib, self.table)
        required = [text for kind, text in gaps if kind == "required"]
        self.assertEqual([], required,
                         'the checker still reports a known `required` '
                         'deviation: %s' % required)
        # The oracle is STRONGER, not weaker: a `required` that is not the
        # document's declared minimum now FAILS the checker instead of being
        # reported.  stdlib §9.6 declares
        # `PadLeft(totalWidth: int, paddingChar: char := ' ')` — `paddingChar`
        # is DEFAULTED, so the declared minimum is 1 and a `required` of 2
        # disagrees with the document.
        mutated = c.apply_mutation(self.table, "string", "PadLeft", 2, 0,
                                   ("required", 2))
        errors = c.compare(self.stdlib, mutated)
        self.assertTrue(errors,
                        'the checker does not FAIL a `required` that is not '
                        'the document\'s declared minimum')
        named = [e for e in errors if "`string.PadLeft`" in e]
        self.assertTrue(named,
                        'the checker fails a corrupted `required` but does '
                        'not NAME the member: %s' % errors)
        self.assertTrue(any("`required` column" in e for e in named),
                        'the checker fails a corrupted `required` but does '
                        'not NAME the column: %s' % errors)

    def test_a_corrupted_required_count_fails_on_every_wrong_value(self):
        # the FileStream row records the document's declared minimum (2);
        # a corruption to any OTHER wrong value must fail.
        for bad in (0, 1, 4, 5):
            mutated = c.apply_mutation(self.table, "FileStream", "", 3, 0,
                                       ("required", bad))
            errors = c.compare(self.stdlib, mutated)
            self.assertTrue(errors,
                            'the checker does not detect a `required` count '
                            'of %d on the FileStream row' % bad)

class NullabilityDeviation(unittest.TestCase):
    """fix #8020: the three §7.3 deviations are RETIRED, and a re-introduced
    one is DETECTED.

    `NULLABILITY_DEVIATION` used to hold three entries — the `StringBuilder`
    positions whose §7.3 (NON-nullable) bit the table left UNASSERTED.  A
    recorded deviation was a SUPPRESSION: the §7.3 comparison did not FAIL on
    it, it was listed in `known_uncovered` as a reported deviation instead.
    So a STALE entry would HIDE a re-introduced deviation at exactly the
    positions the ratchet is asked to cover.

    fix #8020 corrects the TABLE (the `nonNullable` mask of `Insert` and
    `Remove` is 0x03, so the table states the document's own nullability) and
    RETIRES the three entries.  The oracle is therefore STRONGER, not weaker:
    the §7.3 comparison compares the mask against the document with NOTHING
    suppressed, and a deviation at one of those three positions is now a
    FAILURE that names the member and the position.

    THIS CLASS REPLACES A VACUOUS TEST.  The test that stood here iterated
    `NULLABILITY_DEVIATION` — which the fix emptied — so its body ran ZERO
    times and asserted NOTHING.  What is here instead holds whatever the
    registry contains, and FAILS if the checker stops catching a re-introduced
    deviation at these positions.
    """

    # the three positions fix #8020 retired, with the ARITY each row carries
    # in the table (`Insert` and `Remove` are both two-parameter rows).
    RETIRED = (("StringBuilder", "Insert", 0, 2),
               ("StringBuilder", "Remove", 0, 2),
               ("StringBuilder", "Remove", 1, 2))

    @classmethod
    def setUpClass(cls):
        cls.stdlib = c.parse_stdlib(c.read(c.STDLIB), c.read(c.SPEC))
        cls.table = c.parse_table(c.read(c.REGISTRY))

    def test_the_three_retired_deviations_are_gone_and_a_reintroduced_one_fails(self):
        # (1) the three positions are NO LONGER deviations: a recorded
        #     deviation is a SUPPRESSION, so a stale entry would HIDE a
        #     re-introduced deviation at exactly the positions the ratchet is
        #     asked to cover.
        for (head, member, position, _arity) in self.RETIRED:
            self.assertNotIn(
                (head, member, position), c.NULLABILITY_DEVIATION,
                'the §7.3 deviation for `%s.%s` at position %d is still '
                'recorded; a stale suppression would HIDE a re-introduced '
                'deviation at exactly the position the ratchet is asked to '
                'cover' % (head, member, position))
        self.assertEqual(
            {}, dict(c.NULLABILITY_DEVIATION),
            'the §7.3 deviation registry is not empty: %s'
            % sorted(c.NULLABILITY_DEVIATION))
        # (2) the three positions AGREE with the document, so a deviation
        #     there is a real disagreement with the document, not a no-op.
        errors = c.compare(self.stdlib, self.table)
        for (head, member, position, _arity) in self.RETIRED:
            named = [e for e in errors
                     if ("`%s.%s`" % (head, member)) in e
                     and ("position %d" % position) in e]
            self.assertEqual(
                [], named,
                'the table still disagrees with the document at `%s.%s` '
                'position %d: %s'
                % (head, member, position, "; ".join(named)))
        # (3) a RE-INTRODUCED §7.3 deviation at each of them is DETECTED —
        #     the oracle is STRONGER than the base's, which suppressed it.
        for (head, member, position, arity) in self.RETIRED:
            mutated = c.apply_mutation(self.table, head, member, arity,
                                       position, ("nullable",))
            errors = c.compare(self.stdlib, mutated)
            self.assertTrue(
                errors,
                'the checker does not DETECT a re-introduced §7.3 deviation '
                'at `%s.%s` position %d (the table marks it nullable while '
                'the document declares it NON-nullable)'
                % (head, member, position))
            named = [e for e in errors
                     if ("`%s.%s`" % (head, member)) in e
                     and ("position %d" % position) in e]
            self.assertTrue(
                named,
                'the checker detects a re-introduced §7.3 deviation at '
                '`%s.%s` position %d but does not NAME the member and the '
                'position: %s'
                % (head, member, position, "; ".join(errors)))
            self.assertTrue(
                any("NON-nullable" in e for e in named),
                'the checker detects a re-introduced §7.3 deviation at '
                '`%s.%s` position %d but does not name the rule it applies: '
                '%s' % (head, member, position, "; ".join(named)))
        # (4) the checker's own mutation leg still corrupts one of these
        #     positions, so the ratchet proves it can fail.
        self.assertIn(
            ("StringBuilder", "Remove", 2, 0, ("nullable",)),
            c.SELF_TEST_MUTATIONS,
            "the checker's own self-test no longer mutates the §7.3 bit at "
            "`StringBuilder.Remove` position 0")


if __name__ == '__main__':
    unittest.main()
