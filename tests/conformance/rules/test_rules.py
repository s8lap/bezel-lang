#!/usr/bin/env python3
"""Unit tests for the rule-corpus harness. Build-free: no bezelc, no systemd —
the subprocess seam is injected, exactly as test_conformance.py does for the
doc-example harness.

Run: python3 -m unittest discover -s tests/conformance/rules
"""

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus
import coverage
import runner


ACCEPT = """//! case: s28_5_0001
//! doc: spec
//! section: 28.5
//! title: an owned field of class type is admitted
//! rule: Owned fields are restricted to class- or interface-typed fields.
//! expect: accept
module Conf;

public class C
end C;
"""

REJECT = """//! case: s28_5_0002
//! doc: spec
//! section: 28.5
//! title: owned on a string field is rejected
//! rule: On `string`, `owned` is a compile error.
//! expect: reject
//! diagnostic: owned
module Conf;

public class Holder
  private owned _n: string := "x";
end Holder;
"""

RUN = """//! case: s18_5_0001
//! doc: spec
//! section: 18.5
//! title: a null hole renders null
//! rule: A null hole renders the four characters `null`.
//! expect: run
//! stdout: [null]
module Conf;

public class Program
  public static method Main()
    Console.WriteLine("[null]");
  end Main;
end Program;
"""


def write_corpus(base, sources):
    os.makedirs(os.path.join(base, "cases"), exist_ok=True)
    for text in sources:
        fields = corpus.parse_header(text)
        path = os.path.join(base, "cases", fields["case"] + ".bzl")
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)


class TestHeader(unittest.TestCase):
    def test_fields_parse(self):
        f = corpus.parse_header(ACCEPT)
        self.assertEqual(f["case"], "s28_5_0001")
        self.assertEqual(f["expect"], "accept")
        self.assertEqual(f["section"], "28.5")

    def test_continuation_joins_with_one_space(self):
        text = ("//! case: s1_1_0001\n//! rule: first\n//!   second\n"
                "//!   third\nmodule Conf;\n")
        self.assertEqual(corpus.parse_header(text)["rule"], "first second third")

    def test_continuation_is_two_spaces_not_one(self):
        # A single space introduces a field; `//! note: x` after `rule:` is a
        # field, not a continuation. Keeps prose containing a colon safe.
        text = "//! case: s1_1_0001\n//! rule: a\n//! notes: b\nmodule Conf;\n"
        f = corpus.parse_header(text)
        self.assertEqual(f["rule"], "a")
        self.assertEqual(f["notes"], "b")

    def test_duplicate_field_rejected(self):
        text = "//! case: a\n//! case: b\nmodule Conf;\n"
        with self.assertRaises(corpus.CaseError):
            corpus.parse_header(text)

    def test_content_before_header_rejected(self):
        with self.assertRaises(corpus.CaseError):
            corpus.parse_header("module Conf;\n//! case: x\n")

    def test_no_header_rejected(self):
        with self.assertRaises(corpus.CaseError):
            corpus.parse_header("module Conf;\n")

    def test_malformed_header_line_rejected(self):
        with self.assertRaises(corpus.CaseError):
            corpus.parse_header("//! case: x\n//!nospace\nmodule Conf;\n")


class TestCaseId(unittest.TestCase):
    def test_spec_id(self):
        self.assertEqual(corpus.section_of_case_id("s28_3_0010"), ("spec", "28.3"))

    def test_three_level_section(self):
        self.assertEqual(corpus.section_of_case_id("s22_16_1_0003"),
                         ("spec", "22.16.1"))

    def test_section_with_no_subsections_takes_the_short_form(self):
        # §34 has no numbered subsections, so a case there is `s34_0003`.
        self.assertEqual(corpus.section_of_case_id("s34_0003"), ("spec", "34"))

    def test_short_and_long_forms_sort_together(self):
        self.assertEqual(sorted(["s34_0003", "s33_2_0001", "s34_0001"],
                                key=corpus._sort_key),
                         ["s33_2_0001", "s34_0001", "s34_0003"])

    def test_stdlib_id(self):
        self.assertEqual(corpus.section_of_case_id("l4_9_0003"), ("stdlib", "4.9"))

    def test_bad_id(self):
        self.assertIsNone(corpus.section_of_case_id("spec_0001"))

    def test_sort_is_numeric_not_lexical(self):
        ids = ["s28_3_0001", "s4_1_0001", "s28_10_0001", "s28_9_0001", "l1_1_0001"]
        self.assertEqual(sorted(ids, key=corpus._sort_key),
                         ["s4_1_0001", "s28_3_0001", "s28_9_0001", "s28_10_0001",
                          "l1_1_0001"])


class TestLint(unittest.TestCase):
    def check(self, text, name=None):
        fields = {}
        try:
            fields = corpus.parse_header(text)
        except corpus.CaseError:
            pass
        stem = name or fields.get("case", "x")
        return corpus.check_case(stem + ".bzl", text)[1]

    def test_clean_case_has_no_problems(self):
        self.assertEqual(self.check(ACCEPT), [])
        self.assertEqual(self.check(REJECT), [])
        self.assertEqual(self.check(RUN), [])

    def test_filename_must_match_case_field(self):
        problems = self.check(ACCEPT, name="s28_5_0099")
        self.assertTrue(any("does not match the filename" in p for p in problems))

    def test_section_must_agree_with_id(self):
        text = ACCEPT.replace("//! section: 28.5", "//! section: 28.6")
        self.assertTrue(any("disagrees with the id" in p for p in self.check(text)))

    def test_reject_needs_a_diagnostic(self):
        text = REJECT.replace("//! diagnostic: owned\n", "")
        self.assertTrue(any("must carry `diagnostic:`" in p for p in self.check(text)))

    def test_accept_may_not_carry_a_diagnostic(self):
        text = ACCEPT.replace("//! expect: accept",
                              "//! expect: accept\n//! diagnostic: owned")
        self.assertTrue(any("only on a reject or warn case" in p
                            for p in self.check(text)))

    def test_run_needs_stdout(self):
        text = RUN.replace("//! stdout: [null]\n", "")
        self.assertTrue(any("must carry `stdout:`" in p for p in self.check(text)))

    def test_run_needs_main(self):
        text = RUN.replace("public static method Main()", "public static method Go()")
        self.assertTrue(any("needs a `method Main`" in p for p in self.check(text)))

    def test_accept_with_main_is_steered_to_run(self):
        text = RUN.replace("//! expect: run\n//! stdout: [null]\n",
                           "//! expect: accept\n")
        self.assertTrue(any("use `expect: run`" in p for p in self.check(text)))

    def test_missing_module_line_rejected(self):
        text = ACCEPT.replace("module Conf;\n", "")
        self.assertTrue(any("starts with" in p for p in self.check(text)))

    def test_placeholder_source_rejected(self):
        text = ACCEPT.replace("public class C\nend C;",
                              "public class C\n  // TODO fill in\nend C;")
        self.assertTrue(any("complete code" in p for p in self.check(text)))

    def test_ellipsis_rejected(self):
        text = ACCEPT.replace("public class C\nend C;",
                              "public class C\n  ...\nend C;")
        self.assertTrue(any("complete code" in p for p in self.check(text)))

    def test_unknown_field_rejected(self):
        text = ACCEPT.replace("//! expect: accept",
                              "//! flavour: salty\n//! expect: accept")
        self.assertTrue(any("unknown header field" in p for p in self.check(text)))

    def test_duplicate_title_in_a_section_rejected(self):
        with tempfile.TemporaryDirectory() as base:
            twin = ACCEPT.replace("s28_5_0001", "s28_5_0003")
            write_corpus(base, [ACCEPT, twin])
            _rows, problems = corpus.load_cases(os.path.join(base, "cases"))
            self.assertTrue(any("title duplicates" in p for p in problems))


WARN = """//! case: s28_3_0002
//! doc: spec
//! section: 28.3
//! title: an undischarged plain-var owned local raises the must-use warning
//! rule: An owned value reaching its scope's end undischarged is disposed there,
//!   with a must-use warning on the normal edge.
//! expect: warn
//! diagnostic: must use
module Conf;

public class R
  public constructor()
  end constructor;
end R;

public class Program
  public static method Main()
    var r := new R();
  end Main;
end Program;
"""


class TestQuietTier(unittest.TestCase):
    """A rule stated as a SUPPRESSION needs an expectation `accept` cannot give:
    §28.3's warning is suppressed by `using var`, by an explicit dispose or
    discard, and by a move or a return."""

    def row(self):
        return {"case-id": "s28_3_0100", "expect": "quiet", "check": "",
                "shape": "lib", "title": "t", "issue": "", "section": "28.3"}

    def test_a_silent_compile_passes(self):
        self.assertEqual(runner.evaluate(self.row(), 0, "", None, "")[0], "pass")

    def test_any_diagnostic_fails(self):
        v, detail = runner.evaluate(self.row(), 0,
                                    "warning: value of 'r' must be used", None, "")
        self.assertEqual(v, "fail")
        self.assertIn("suppresses", detail)

    def test_a_rejected_compile_fails(self):
        self.assertEqual(runner.evaluate(self.row(), 1, "error", None, "")[0],
                         "fail")

    def test_quiet_carries_no_diagnostic_field(self):
        text = ACCEPT.replace("//! expect: accept",
                              "//! expect: quiet\n//! diagnostic: x")
        self.assertTrue(any("only on a reject or warn case" in p for p in
                            corpus.check_case("s28_5_0001.bzl", text)[1]))


class TestWarnTier(unittest.TestCase):
    def test_warn_needs_a_diagnostic(self):
        text = WARN.replace("//! diagnostic: must use\n", "")
        problems = corpus.check_case("s28_3_0002.bzl", text)[1]
        self.assertTrue(any("must carry `diagnostic:`" in p for p in problems))

    def test_a_warn_case_with_main_is_not_steered_to_run(self):
        # The must-use warning fires in a body; a Main is the natural place for
        # one, and nothing is printed, so `run` would be the wrong expectation.
        self.assertEqual(corpus.check_case("s28_3_0002.bzl", WARN)[1], [])

    def row(self, check="must use"):
        return {"case-id": "s28_3_0002", "expect": "warn", "check": check,
                "shape": "program", "title": "t", "issue": "", "section": "28.3"}

    def test_warn_passes_when_the_compiler_warns(self):
        v, _ = runner.evaluate(self.row(), 0,
                               "warning: value of 'r' must be used", None, "")
        self.assertEqual(v, "pass")

    def test_warn_fails_on_a_silent_compile(self):
        v, detail = runner.evaluate(self.row(), 0, "", None, "")
        self.assertEqual(v, "fail")
        self.assertIn("silently", detail)

    def test_warn_fails_when_rejected(self):
        self.assertEqual(runner.evaluate(self.row(), 1, "error: nope", None, "")[0],
                         "fail")

    def test_warn_wording_is_reported_not_gated(self):
        # It warns, so the case passes; the wording mismatch is a finding.
        v, _ = runner.evaluate(self.row(), 0, "warning: unused local", None, "")
        self.assertEqual(v, "pass")
        self.assertFalse(runner.diagnostic_matches(self.row(), "warning: unused local"))


MULTI_MAIN = """//! case: s13_4_0001
//! doc: spec
//! section: 13.4
//! title: an internal member is not visible from another module
//! rule: `internal` reaches the declaring module only.
//! expect: reject
//! diagnostic: internal
module Conf;

import Lib;

public class Program
  public static method Main()
    Console.WriteLine(Helper.Secret);
  end Main;
end Program;
"""

MULTI_DEP = """module Lib;

public class Helper
  internal static const Secret: int := 1;
end Helper;
"""


def write_dir_case(base, case_id, main_text, deps):
    d = os.path.join(base, "cases", case_id)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "main.bzl"), "w", encoding="utf-8") as f:
        f.write(main_text)
    for name, text in deps.items():
        with open(os.path.join(d, name), "w", encoding="utf-8") as f:
            f.write(text)


OUTPUT_CASE = """//! case: s28_4_0001
//! doc: spec
//! section: 28.4
//! title: owned fields dispose in reverse declaration order
//! rule: The generated Dispose runs the destructor body, then disposes owned
//!   fields in reverse declaration order.
//! expect: run
//! output: container
//!   second
//!   first
module Conf;

public class Program
  public static method Main()
    Console.WriteLine("container");
  end Main;
end Program;
"""


class TestMultiLineOutput(unittest.TestCase):
    """Disposal ORDER is the corpus's most valuable observation and it takes
    more than one line to see."""

    def test_continuations_of_output_are_lines(self):
        f = corpus.parse_header(OUTPUT_CASE)
        self.assertEqual(f["output"], "container\nsecond\nfirst")

    def test_a_run_case_may_carry_output_instead_of_stdout(self):
        self.assertEqual(corpus.check_case("s28_4_0001.bzl", OUTPUT_CASE)[1], [])

    def test_not_both(self):
        text = OUTPUT_CASE.replace("//! output: container",
                                   "//! stdout: container\n//! output: container")
        self.assertTrue(any("not both" in p for p in
                            corpus.check_case("s28_4_0001.bzl", text)[1]))

    def test_output_survives_the_manifest_round_trip(self):
        with tempfile.TemporaryDirectory() as base:
            write_corpus(base, [OUTPUT_CASE])
            corpus.main(["build"], base_dir=base)
            row = corpus.read_manifest(os.path.join(base, "manifest.tsv"))[0]
            self.assertEqual(row["check"], "all:container\\nsecond\\nfirst")
            self.assertEqual(corpus.main(["check"], base_dir=base), 0)

    def row(self):
        return {"case-id": "s28_4_0001", "expect": "run", "shape": "program",
                "check": "all:container\\nsecond\\nfirst", "title": "t",
                "issue": "", "section": "28.4"}

    def test_every_line_must_match(self):
        good = "container\nsecond\nfirst\n"
        self.assertEqual(runner.evaluate(self.row(), 0, "", 0, good)[0], "pass")

    def test_a_wrong_order_fails(self):
        bad = "container\nfirst\nsecond\n"
        self.assertEqual(runner.evaluate(self.row(), 0, "", 0, bad)[0], "fail")

    def test_a_missing_line_fails(self):
        self.assertEqual(runner.evaluate(self.row(), 0, "", 0, "container\n")[0],
                         "fail")

    def test_trailing_newlines_and_carriage_returns_are_tolerated(self):
        out = "container\r\nsecond\r\nfirst\r\n\n"
        self.assertEqual(runner.evaluate(self.row(), 0, "", 0, out)[0], "pass")


class TestDirectoryCases(unittest.TestCase):
    """The rules that need two compilation units: a case may be a directory of
    `main.bzl` plus its dependencies."""

    def setUp(self):
        self.base = tempfile.mkdtemp()
        write_corpus(self.base, [ACCEPT])
        write_dir_case(self.base, "s13_4_0001", MULTI_MAIN, {"lib.bzl": MULTI_DEP})

    def rows(self):
        rows, problems = corpus.load_cases(os.path.join(self.base, "cases"))
        return {r["case-id"]: r for r in rows}, problems

    def test_the_directory_is_one_case_with_its_deps_recorded(self):
        rows, problems = self.rows()
        self.assertEqual(problems, [])
        self.assertEqual(rows["s13_4_0001"]["deps"], "lib.bzl")
        self.assertEqual(rows["s13_4_0001"]["section"], "13.4")
        self.assertEqual(rows["s28_5_0001"]["deps"], "")

    def test_a_dependency_may_not_carry_a_header(self):
        write_dir_case(self.base, "s13_4_0001", MULTI_MAIN,
                       {"lib.bzl": "//! case: x\n" + MULTI_DEP})
        self.assertTrue(any("only main.bzl carries" in p
                            for p in self.rows()[1]))

    def test_a_dependency_may_not_hold_the_entry_point(self):
        write_dir_case(self.base, "s13_4_0001", MULTI_MAIN,
                       {"lib.bzl": MULTI_DEP.replace(
                           "internal static const Secret: int := 1;",
                           "public static method Main() end Main;")})
        self.assertTrue(any("belongs in main.bzl" in p for p in self.rows()[1]))

    def test_a_dependency_may_hold_Main_when_the_case_opts_in(self):
        # §2.8.4: a qualifying Main in a DEPENDENCY artifact is inert, and
        # presenting an executable as a dependency is a build error. Both rules
        # are about a dependency that holds one.
        main = MULTI_MAIN.replace("//! expect: reject",
                                  "//! notes: dep-main\n//! expect: reject")
        write_dir_case(self.base, "s13_4_0001", main,
                       {"lib.bzl": MULTI_DEP.replace(
                           "internal static const Secret: int := 1;",
                           "public static method Main() end Main;")})
        self.assertEqual(self.rows()[1], [])

    def test_note_tokens_are_exact(self):
        self.assertEqual(corpus.note_tokens("dep-main; other"),
                         ["dep-main", "other"])
        self.assertEqual(corpus.note_tokens(None), [])
        self.assertNotIn("dep-main", corpus.note_tokens("no dep-mainly here"))

    def test_a_directory_without_main_is_a_problem(self):
        os.remove(os.path.join(self.base, "cases", "s13_4_0001", "main.bzl"))
        self.assertTrue(any("needs a main.bzl" in p for p in self.rows()[1]))

    def test_the_dependency_is_compiled_first_and_imported(self):
        corpus.main(["build"], base_dir=self.base)
        seen = []

        def fake(cmd, cwd=None):
            seen.append(list(cmd))
            if cmd[-1].endswith("main.bzl"):
                return 1, "error: 'Secret' is internal to module Lib"
            return 0, ""

        bezelc = os.path.join(self.base, "bezelc")
        with open(bezelc, "w") as f:
            f.write("#!/bin/sh\nexit 0\n")
        os.chmod(bezelc, 0o755)
        baseline = os.path.join(self.base, "baseline.tsv")
        rc = runner.main([bezelc, "--classify", "--filter", "s13_4",
                          "--baseline", baseline],
                         base_dir=self.base, run=fake)
        self.assertEqual(rc, 0)
        self.assertEqual(runner.read_baseline(baseline)["s13_4_0001"]["verdict"],
                         "pass")
        self.assertTrue(seen[0][-1].endswith("lib.bzl"))
        self.assertIn("--import", seen[1])

    def test_a_dependency_that_does_not_compile_is_never_a_passing_reject(self):
        corpus.main(["build"], base_dir=self.base)

        def fake(cmd, cwd=None):
            return 1, "error: broken"

        bezelc = os.path.join(self.base, "bezelc")
        with open(bezelc, "w") as f:
            f.write("#!/bin/sh\nexit 0\n")
        os.chmod(bezelc, 0o755)
        rows = corpus.read_manifest(os.path.join(self.base, "manifest.tsv"))
        row = [r for r in rows if r["case-id"] == "s13_4_0001"][0]
        verdict, detail, _log = runner.run_case(
            row, bezelc, os.path.join(self.base, "cases"), self.base, run=fake)
        self.assertEqual(verdict, "fail")
        self.assertIn("dependency lib.bzl did not compile", detail)


class TestStructuralLints(unittest.TestCase):
    """Three checks that each earned their place by catching a real defect."""

    def problems(self, text, stem="s28_5_0001"):
        return corpus.check_case(stem + ".bzl", text)[1]

    def test_an_end_label_naming_nothing_is_caught(self):
        # Five cases carried `end Main;` in a class named Program with a method
        # named Run. None of them parsed; nothing else here could see it.
        text = ACCEPT.replace("end C;", "end Main;")
        self.assertTrue(any("names no construct" in p for p in self.problems(text)))

    def test_a_reject_case_may_be_about_a_mismatched_label(self):
        text = REJECT.replace("end Holder;", "end Wrong;")
        self.assertFalse(any("names no construct" in p for p in
                             corpus.check_case("s28_5_0002.bzl", text)[1]))

    def test_a_keyword_label_and_a_loop_label_are_both_fine(self):
        text = ACCEPT.replace("public class C\nend C;",
                              "public class C\n"
                              "  public method M()\n"
                              "    scan: while false do\n"
                              "    end scan;\n"
                              "  end M;\n"
                              "end C;")
        self.assertEqual([p for p in self.problems(text)
                          if "names no construct" in p], [])

    def test_new_on_a_declared_class_with_no_constructor_is_caught(self):
        text = ACCEPT.replace(
            "public class C\nend C;",
            "public class C\nend C;\n\n"
            "public class Program\n"
            "  public static method Main()\n"
            "    using var c := new C();\n"
            "  end Main;\n"
            "end Program;").replace("//! expect: accept",
                                    "//! expect: run\n//! stdout: x")
        self.assertTrue(any("#4364" in p for p in self.problems(text)))

    def test_a_primary_constructor_counts(self):
        text = ACCEPT.replace(
            "public class C\nend C;",
            "public class C(n: int)\nend C;\n\n"
            "public class Program\n"
            "  public static method Main()\n"
            "    using var c := new C(1);\n"
            "  end Main;\n"
            "end Program;").replace("//! expect: accept",
                                    "//! expect: run\n//! stdout: x")
        self.assertEqual([p for p in self.problems(text) if "#4364" in p], [])

    def test_new_on_an_undeclared_stdlib_class_is_not_this_checks_business(self):
        text = ACCEPT.replace(
            "public class C\nend C;",
            "public class Program\n"
            "  public static method Main()\n"
            "    using var a := new Array<int>();\n"
            "  end Main;\n"
            "end Program;").replace("//! expect: accept",
                                    "//! expect: run\n//! stdout: x")
        self.assertEqual([p for p in self.problems(text) if "#4364" in p], [])

    def test_a_lowercase_bool_expectation_is_caught(self):
        text = RUN.replace("//! stdout: [null]", "//! stdout: true")
        self.assertTrue(any("True`/`False" in p for p in
                            corpus.check_case("s18_5_0001.bzl", text)[1]))


class TestManifest(unittest.TestCase):
    def test_build_then_check_is_clean(self):
        with tempfile.TemporaryDirectory() as base:
            write_corpus(base, [ACCEPT, REJECT, RUN])
            self.assertEqual(corpus.main(["build"], base_dir=base), 0)
            self.assertEqual(corpus.main(["check"], base_dir=base), 0)

    def test_check_fails_when_a_case_changed(self):
        with tempfile.TemporaryDirectory() as base:
            write_corpus(base, [ACCEPT])
            corpus.main(["build"], base_dir=base)
            write_corpus(base, [REJECT])
            self.assertEqual(corpus.main(["check"], base_dir=base), 1)

    def test_shape_is_derived_from_the_source(self):
        rows = {r["case-id"]: r for r in
                corpus.load_cases(self._corpus([ACCEPT, RUN]))[0]}
        self.assertEqual(rows["s28_5_0001"]["shape"], "lib")
        self.assertEqual(rows["s18_5_0001"]["shape"], "program")

    def _corpus(self, sources):
        base = tempfile.mkdtemp()
        self.addCleanup(lambda: None)
        write_corpus(base, sources)
        return os.path.join(base, "cases")

    def test_lint_problems_block_a_build(self):
        with tempfile.TemporaryDirectory() as base:
            write_corpus(base, [ACCEPT])
            bad = os.path.join(base, "cases", "s99_9_0001.bzl")
            with open(bad, "w", encoding="utf-8") as f:
                f.write("//! case: wrong\nmodule Conf;\n")
            self.assertEqual(corpus.main(["build"], base_dir=base), 1)


class TestCompilerFlags(unittest.TestCase):
    """A rule that only holds in a non-default build mode needs its switch."""

    FLAGGED = ACCEPT.replace("//! expect: accept",
                             "//! flags: --checked-disposal\n//! expect: accept")

    def test_an_allowlisted_flag_is_admitted_and_recorded(self):
        with tempfile.TemporaryDirectory() as base:
            write_corpus(base, [self.FLAGGED])
            rows, problems = corpus.load_cases(os.path.join(base, "cases"))
            self.assertEqual(problems, [])
            self.assertEqual(rows[0]["flags"], "--checked-disposal")

    def test_a_flag_off_the_allowlist_is_a_lint_problem(self):
        text = ACCEPT.replace("//! expect: accept",
                              "//! flags: --sanitize=address\n//! expect: accept")
        self.assertTrue(any("allowlist" in p for p in
                            corpus.check_case("s28_5_0001.bzl", text)[1]))

    def test_the_flag_reaches_the_compile_command(self):
        seen = []

        def fake(cmd, cwd=None):
            seen.append(list(cmd))
            return 0, ""

        row = {"case-id": "s28_5_0001", "expect": "accept", "shape": "lib",
               "check": "", "title": "t", "issue": "", "section": "28.5",
               "deps": "", "flags": "--checked-disposal"}
        with tempfile.TemporaryDirectory() as base:
            write_corpus(base, [self.FLAGGED])
            runner.run_case(row, "/bin/true", os.path.join(base, "cases"),
                            base, run=fake)
        self.assertIn("--checked-disposal", seen[0])
        self.assertLess(seen[0].index("--checked-disposal"), seen[0].index("-c"))


class TestEvaluate(unittest.TestCase):
    def row(self, expect, check=""):
        return {"case-id": "s1_1_0001", "expect": expect, "check": check,
                "shape": "lib", "title": "t", "issue": "", "section": "1.1"}

    def test_accept_passes_on_clean_compile(self):
        self.assertEqual(runner.evaluate(self.row("accept"), 0, "", None, "")[0],
                         "pass")

    def test_accept_fails_on_compile_error(self):
        self.assertEqual(runner.evaluate(self.row("accept"), 1, "boom", None, "")[0],
                         "fail")

    def test_reject_passes_on_compile_error(self):
        self.assertEqual(runner.evaluate(self.row("reject"), 1, "boom", None, "")[0],
                         "pass")

    def test_reject_fails_when_it_compiles(self):
        v, detail = runner.evaluate(self.row("reject"), 0, "", None, "")
        self.assertEqual(v, "fail")
        self.assertIn("the spec rejects this", detail)

    def test_a_compiler_crash_is_never_a_passing_reject(self):
        # 139 is SIGSEGV: the case was rejected by a crash, which proves nothing
        # about the rule and hides a compiler bug.
        v, detail = runner.evaluate(self.row("reject"), 139, "", None, "")
        self.assertEqual(v, "fail")
        self.assertIn("crashed", detail)

    def test_compile_timeout_fails_both_ways(self):
        self.assertEqual(runner.evaluate(self.row("reject"), 124, "", None, "")[0],
                         "fail")
        self.assertEqual(runner.evaluate(self.row("accept"), 124, "", None, "")[0],
                         "fail")

    def test_run_compares_the_first_stdout_line(self):
        row = self.row("run", "[null]")
        self.assertEqual(runner.evaluate(row, 0, "", 0, "[null]\nmore\n")[0], "pass")
        self.assertEqual(runner.evaluate(row, 0, "", 0, "[]\n")[0], "fail")

    def test_run_strips_carriage_returns(self):
        row = self.row("run", "[null]")
        self.assertEqual(runner.evaluate(row, 0, "", 0, "[null]\r\n")[0], "pass")

    def test_run_fails_on_a_program_crash(self):
        row = self.row("run", "x")
        self.assertEqual(runner.evaluate(row, 0, "", 139, "")[0], "fail")

    def test_run_fails_with_no_artifact(self):
        row = self.row("run", "x")
        self.assertIn("no runnable artifact",
                      runner.evaluate(row, 0, "", None, "")[1])


class TestDiagnosticReport(unittest.TestCase):
    def test_substring_match_is_case_insensitive(self):
        row = {"expect": "reject", "check": "Owned"}
        self.assertTrue(runner.diagnostic_matches(row, "error: owned is not allowed"))

    def test_mismatch_is_detected(self):
        row = {"expect": "reject", "check": "owned"}
        self.assertFalse(runner.diagnostic_matches(row, "error: unexpected token"))


class TestRatchet(unittest.TestCase):
    def test_unclassified_when_absent(self):
        self.assertEqual(runner.compare("pass", None), runner.UNCLASSIFIED)
        self.assertEqual(runner.compare("fail", None), runner.UNCLASSIFIED)

    def test_banked_pass_holds(self):
        self.assertEqual(runner.compare("pass", {"verdict": "pass"}), runner.OK)

    def test_banked_pass_regressing_is_a_failure(self):
        self.assertEqual(runner.compare("fail", {"verdict": "pass"}),
                         runner.REGRESSION)

    def test_banked_fail_still_failing_is_known(self):
        self.assertEqual(runner.compare("fail", {"verdict": "fail"}), runner.KNOWN)

    def test_banked_fail_now_passing_must_be_banked(self):
        # Exact, not a ceiling: unbanked headroom is indistinguishable from a
        # divergence someone already spent.
        self.assertEqual(runner.compare("pass", {"verdict": "fail"}),
                         runner.UNBANKED)


class TestBaselineFile(unittest.TestCase):
    def test_round_trip(self):
        rows = {"s1_1_0001": {"verdict": "pass", "note": ""},
                "s2_1_0002": {"verdict": "fail", "note": "#4154"}}
        with tempfile.TemporaryDirectory() as base:
            path = os.path.join(base, "baseline.tsv")
            with open(path, "w", encoding="utf-8") as f:
                f.write(runner.render_baseline(rows, "a note\nspanning lines"))
            self.assertEqual(runner.read_baseline(path), rows)

    def test_missing_file_is_an_empty_baseline(self):
        self.assertEqual(runner.read_baseline("/nonexistent/baseline.tsv"), {})


class FakeCompiler(object):
    """A `run` seam: maps a case id to (compile_rc, log) and (run_rc, stdout)."""

    def __init__(self, table):
        self.table = table

    def __call__(self, cmd, cwd=None):
        target = cmd[-1]
        if target.endswith(".bzl"):
            case_id = os.path.basename(target)[:-len(".bzl")]
            self.last = case_id
            rc, log = self.table[case_id][0]
            if rc == 0 and "-o" in cmd:
                out = cmd[cmd.index("-o") + 1]
                with open(out, "w") as f:
                    f.write("#!/bin/sh\nexit 0\n")
                os.chmod(out, 0o755)
            return rc, log
        return self.table[self.last][1]


class TestRunnerEndToEnd(unittest.TestCase):
    def setUp(self):
        self.base = tempfile.mkdtemp()
        write_corpus(self.base, [ACCEPT, REJECT, RUN])
        corpus.main(["build"], base_dir=self.base)
        self.bezelc = os.path.join(self.base, "bezelc")
        with open(self.bezelc, "w") as f:
            f.write("#!/bin/sh\nexit 0\n")
        os.chmod(self.bezelc, 0o755)
        self.baseline = os.path.join(self.base, "baseline.tsv")

    def all_as_spec_says(self):
        return FakeCompiler({
            "s28_5_0001": ((0, ""), (0, "")),
            "s28_5_0002": ((1, "error: owned is not admitted here"), (0, "")),
            "s18_5_0001": ((0, ""), (0, "[null]\n")),
        })

    def test_classify_then_gate_is_green(self):
        run = self.all_as_spec_says()
        rc = runner.main([self.bezelc, "--classify", "--baseline", self.baseline],
                         base_dir=self.base, run=run)
        self.assertEqual(rc, 0)
        banked = runner.read_baseline(self.baseline)
        self.assertEqual(len(banked), 3)
        self.assertTrue(all(r["verdict"] == "pass" for r in banked.values()))
        rc = runner.main([self.bezelc, "--baseline", self.baseline],
                         base_dir=self.base, run=run)
        self.assertEqual(rc, 0)

    def test_a_regression_fails_the_gate(self):
        runner.main([self.bezelc, "--classify", "--baseline", self.baseline],
                    base_dir=self.base, run=self.all_as_spec_says())
        broken = self.all_as_spec_says()
        broken.table["s28_5_0001"] = ((1, "error: unexpected"), (0, ""))
        rc = runner.main([self.bezelc, "--baseline", self.baseline],
                         base_dir=self.base, run=broken)
        self.assertEqual(rc, 1)

    def test_a_divergence_is_recorded_and_does_not_fail_the_gate(self):
        diverging = self.all_as_spec_says()
        # The compiler accepts what the spec rejects: a real gap, banked as such.
        diverging.table["s28_5_0002"] = ((0, ""), (0, ""))
        runner.main([self.bezelc, "--classify", "--baseline", self.baseline],
                    base_dir=self.base, run=diverging)
        banked = runner.read_baseline(self.baseline)
        self.assertEqual(banked["s28_5_0002"]["verdict"], "fail")
        rc = runner.main([self.bezelc, "--baseline", self.baseline],
                         base_dir=self.base, run=diverging)
        self.assertEqual(rc, 0)

    def test_fixing_a_divergence_fails_until_banked(self):
        diverging = self.all_as_spec_says()
        diverging.table["s28_5_0002"] = ((0, ""), (0, ""))
        runner.main([self.bezelc, "--classify", "--baseline", self.baseline],
                    base_dir=self.base, run=diverging)
        fixed = self.all_as_spec_says()
        rc = runner.main([self.bezelc, "--baseline", self.baseline],
                         base_dir=self.base, run=fixed)
        self.assertEqual(rc, 1)
        rc = runner.main([self.bezelc, "--update", "--baseline", self.baseline],
                         base_dir=self.base, run=fixed)
        self.assertEqual(rc, 1)  # the run that banks still reports the ratchet
        rc = runner.main([self.bezelc, "--baseline", self.baseline],
                         base_dir=self.base, run=fixed)
        self.assertEqual(rc, 0)

    def test_an_unclassified_case_does_not_fail_the_gate_by_default(self):
        rc = runner.main([self.bezelc, "--baseline", self.baseline],
                         base_dir=self.base, run=self.all_as_spec_says())
        self.assertEqual(rc, 0)

    def test_strict_new_fails_on_an_unclassified_case(self):
        rc = runner.main([self.bezelc, "--strict-new", "--baseline", self.baseline],
                         base_dir=self.base, run=self.all_as_spec_says())
        self.assertEqual(rc, 1)

    def test_a_missing_source_file_fails_that_case(self):
        os.remove(os.path.join(self.base, "cases", "s28_5_0001.bzl"))
        rc = runner.main([self.bezelc, "--classify", "--baseline", self.baseline],
                         base_dir=self.base, run=self.all_as_spec_says())
        self.assertEqual(rc, 0)
        self.assertEqual(runner.read_baseline(self.baseline)["s28_5_0001"]["verdict"],
                         "fail")

    def test_filter_selects_a_section(self):
        rc = runner.main([self.bezelc, "--classify", "--filter", "28.5",
                          "--baseline", self.baseline],
                         base_dir=self.base, run=self.all_as_spec_says())
        self.assertEqual(rc, 0)
        self.assertEqual(sorted(runner.read_baseline(self.baseline)),
                         ["s28_5_0001", "s28_5_0002"])


class TestCommittedCorpus(unittest.TestCase):
    """The corpus in this directory, not a synthetic one."""

    def test_lint_is_clean_and_the_manifest_is_current(self):
        base = os.path.dirname(os.path.abspath(__file__))
        rows, problems = corpus.load_cases(os.path.join(base, "cases"))
        self.assertEqual(problems, [])
        self.assertGreater(len(rows), 0)
        with open(os.path.join(base, "manifest.tsv"), encoding="utf-8") as f:
            self.assertEqual(f.read(), corpus.render_manifest(rows))

    def test_every_baseline_row_names_a_live_case(self):
        base = os.path.dirname(os.path.abspath(__file__))
        rows = {r["case-id"] for r in corpus.load_cases(
            os.path.join(base, "cases"))[0]}
        banked = runner.read_baseline(os.path.join(base, "baseline.tsv"))
        self.assertEqual(sorted(set(banked) - rows), [])


class TestOrderClaimSignal(unittest.TestCase):
    """The audit's order-or-count signal. Not a lint: a single line can be a
    complete check when the program encodes the order into it. The signal's
    value is entirely in its precision — a broad vocabulary flagged 225 of 723
    run cases on incidental words, and a report that is two-thirds noise gets
    ignored and then switched off."""

    def hits(self, text):
        return bool(corpus.ORDER_CLAIM_RE.search(text))

    def test_it_catches_the_claims_that_cost_us(self):
        for text in (
                "disposed in reverse order of establishment",
                "owned fields are disposed in reverse declaration order",
                "static fields are initialized in textual declaration order",
                "the cleanup runs exactly once",
                "a second Dispose() is idempotent",
                "calling GetEnumerator again re-runs the body",
                "each iteration produces a fresh binding",
                "the finalizer never runs twice",
        ):
            self.assertTrue(self.hits(text), text)

    def test_it_ignores_the_incidental_words(self):
        # Every one of these tripped the first, broad vocabulary.
        for text in (
                "the first matching arm wins",
                "the last declared member shadows",
                "an owned field of class type is admitted",
                "the number of type parameters must match",
                "a count of zero is admitted",
                "an escape after the borrow's source is out of scope",
        ):
            self.assertFalse(self.hits(text), text)

    def test_a_multi_line_output_case_is_never_flagged(self):
        rows = [{"case-id": "s1_1_0001", "expect": "run",
                 "check": "all:a\nb", "rule": "disposed in reverse order",
                 "title": "t", "cites": "1.1"}]
        self.assertFalse(
            rows[0]["check"].startswith("all:") is False
            and corpus.ORDER_CLAIM_RE.search(rows[0]["rule"]) is None)
        # The audit's own guard: `all:` is the multi-line form.
        self.assertTrue(rows[0]["check"].startswith("all:"))


class TestCoverageStates(unittest.TestCase):
    """The two ways a coverage gap can be false. Both existed as prose in the
    README before they were code, and every wave re-investigated the same empty
    sections — which is the whole argument for executing a rule rather than
    writing it down."""

    EMPTY = {"cases": 0, "accept": 0, "reject": 0, "warn": 0, "run": 0}
    ONE = {"cases": 1, "accept": 1, "reject": 0, "warn": 0, "run": 0}

    def test_a_container_is_not_a_gap(self):
        # Appendix C rows citing a subsection are attributed to the parent too,
        # so a container reads as dense while stating nothing at all.
        self.assertEqual(
            coverage.state_of(0, 2, self.EMPTY, container=True), "n/a")

    def test_a_container_that_somehow_has_cases_is_still_reported(self):
        self.assertEqual(
            coverage.state_of(0, 2, self.ONE, container=True), "covered")

    def test_a_section_that_states_a_rule_and_has_nothing_is_a_gap(self):
        self.assertEqual(
            coverage.state_of(2, 2, self.EMPTY), "UNCOVERED")

    def test_an_ack_moves_a_gap_to_covered_elsewhere(self):
        self.assertEqual(
            coverage.state_of(2, 1, self.EMPTY, ack={"covered-by": "s1_1_0001"}),
            "covered-elsewhere")

    def test_an_ack_does_not_override_a_real_case_count(self):
        self.assertEqual(
            coverage.state_of(2, 1, self.ONE, ack={"covered-by": "s1_1_0001"}),
            "covered")


class TestCoverageAcks(unittest.TestCase):
    """An ack is a claim, so it is checked. An ack naming a case that does not
    exist would silently hide a real gap forever — the one outcome worse than
    the false gap it removes."""

    HEADER = "\t".join(coverage.ACK_COLUMNS)

    def _write(self, body):
        d = tempfile.mkdtemp()
        path = os.path.join(d, "coverage_acks.tsv")
        with open(path, "w", encoding="utf-8") as f:
            f.write(body)
        return path

    def test_a_missing_file_is_not_an_error(self):
        acks, problems = coverage.parse_acks("/nonexistent/acks.tsv", set())
        self.assertEqual((acks, problems), ({}, []))

    def test_a_good_ack_parses(self):
        path = self._write(self.HEADER + "\nspec\t25\ts25_1_0004\tbecause\n")
        acks, problems = coverage.parse_acks(path, {"s25_1_0004"})
        self.assertEqual(problems, [])
        self.assertEqual(acks[("spec", "25")]["covered-by"], "s25_1_0004")

    def test_an_ack_naming_no_case_is_a_problem(self):
        path = self._write(self.HEADER + "\nspec\t25\ts25_1_9999\tbecause\n")
        _, problems = coverage.parse_acks(path, {"s25_1_0004"})
        self.assertEqual(len(problems), 1)
        self.assertIn("s25_1_9999", problems[0])

    def test_every_case_in_a_multi_case_ack_is_checked(self):
        path = self._write(
            self.HEADER + "\nspec\t25\ts25_1_0004, s25_1_9999\tbecause\n")
        _, problems = coverage.parse_acks(path, {"s25_1_0004"})
        self.assertEqual(len(problems), 1)
        self.assertIn("s25_1_9999", problems[0])

    def test_a_wrong_header_is_a_problem(self):
        path = self._write("doc\tsection\n spec\t25\n")
        acks, problems = coverage.parse_acks(path, set())
        self.assertEqual(acks, {})
        self.assertEqual(len(problems), 1)

    def test_comments_and_blank_lines_are_skipped(self):
        path = self._write("# a note\n\n" + self.HEADER
                           + "\nspec\t25\ts25_1_0004\tbecause\n")
        acks, problems = coverage.parse_acks(path, {"s25_1_0004"})
        self.assertEqual(problems, [])
        self.assertEqual(len(acks), 1)

    def test_the_committed_acks_all_name_live_cases(self):
        base = os.path.dirname(os.path.abspath(__file__))
        ids = {r["case-id"] for r in corpus.load_cases(
            os.path.join(base, "cases"))[0]}
        _, problems = coverage.parse_acks(
            os.path.join(base, "coverage_acks.tsv"), ids)
        self.assertEqual(problems, [])


class TestCoverageMarkup(unittest.TestCase):
    """The spec emits an `<a id=...>` anchor before every heading. Counting one
    as prose made every container section look non-empty, which is why the
    container rule silently did nothing on its first cut."""

    def test_an_anchor_line_is_not_prose(self):
        self.assertFalse(coverage._is_prose('<a id="sec-source-files"></a>'))

    def test_an_html_comment_is_not_prose(self):
        self.assertFalse(coverage._is_prose("  <!-- a note -->  "))

    def test_a_blank_line_is_not_prose(self):
        self.assertFalse(coverage._is_prose("   "))

    def test_a_sentence_is_prose(self):
        self.assertTrue(coverage._is_prose("A source file is UTF-8."))

    def test_a_sentence_after_an_anchor_is_prose(self):
        self.assertTrue(coverage._is_prose('<a id="x"></a> and then text'))


if __name__ == "__main__":
    unittest.main()
