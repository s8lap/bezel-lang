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


if __name__ == "__main__":
    unittest.main()
