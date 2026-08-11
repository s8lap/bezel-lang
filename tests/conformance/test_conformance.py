"""Unit tests for the conformance harness port (per Plan-A 5.1; #3010).

Mirrors the scripts/gate/tests pattern: stdlib unittest only, sys.path adjusted
so `import conformance` resolves to the sibling module. The wrap heuristics,
preamble splice, crash classification, expectation evaluation, and manifest
reading are pure-function pins (each branch of the bash cascade exercised); the
run_case decision tree is driven through an injected fake `run` so no bezelc or
systemd-run is ever invoked. The load-bearing test is the PASS>0 silent-green
guard — the exact gap Plan-A 5.1 names: a sweep that validated NOTHING must FAIL
LOUD, not exit 0.
"""

import io
import os
import stat
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import conformance as cf  # noqa: E402


# -----------------------------------------------------------------------
# first_nonblank_line / classify_wrap — the heuristic cascade.
# -----------------------------------------------------------------------

class FirstNonblankTest(unittest.TestCase):
    def test_returns_first_non_blank_non_slash_line(self):
        self.assertEqual(cf.first_nonblank_line("\n\n  class A\nend\n"),
                         "  class A")

    def test_skips_slash_comment_leaders(self):
        # A leading `// comment` line does not qualify (the bash grep pattern
        # excludes lines whose first non-space char is `/`).
        self.assertEqual(cf.first_nonblank_line("// hi\nfoo: int\n"), "foo: int")

    def test_none_when_all_blank_or_comments(self):
        self.assertIsNone(cf.first_nonblank_line("\n   \n//only\n"))


class ClassifyWrapTest(unittest.TestCase):
    def _mode(self, code):
        return cf.classify_wrap(code)[0]

    def test_module_with_main_is_as_is_program(self):
        code = "module X;\nclass C\n  method Main()\n  end Main;\nend C;\n"
        self.assertEqual(self._mode(code), cf.WRAP_AS_IS_PROGRAM)

    def test_module_without_main_is_as_is_lib(self):
        code = "module X;\nclass C\n  method F() end F;\nend C;\n"
        self.assertEqual(self._mode(code), cf.WRAP_AS_IS_LIB)

    def test_type_declaration_wraps_as_lib(self):
        code = "public class Connection\n  constructor() end constructor;\nend Connection;\n"
        mode, wrapped = cf.classify_wrap(code)
        self.assertEqual(mode, cf.WRAP_LIB)
        self.assertTrue(wrapped.startswith("module Conf;\n\n"))
        self.assertIn(code, wrapped)

    def test_record_type_declaration(self):
        self.assertEqual(self._mode("public record Point(X: int, Y: int);\n"),
                         cf.WRAP_LIB)

    def test_extension_block_wraps_as_lib(self):
        code = "extension int\n  method Doubled() => this * 2;\nend int;\n"
        mode, wrapped = cf.classify_wrap(code)
        self.assertEqual(mode, cf.WRAP_LIB)
        self.assertTrue(wrapped.startswith("module Conf;\n\n"))

    # The three TopDecl forms added at the v0.247 corpus regeneration
    # (spec Appendix A.1: ChoiceDecl | TypeAliasDecl | ExternBlock).
    def test_choice_declaration_wraps_as_lib(self):
        code = "public choice Result<T, E>\n  Ok(owned value: T),\n  Error(owned error: E);\nend Result;\n"
        self.assertEqual(self._mode(code), cf.WRAP_LIB)

    def test_type_alias_wraps_as_lib(self):
        code = "type IntPredicate := Predicate<int>;\n"
        self.assertEqual(self._mode(code), cf.WRAP_LIB)

    def test_extern_block_wraps_as_lib(self):
        code = 'extern "libc"\n  public method puts(s: CString): int;\nend;\n'
        self.assertEqual(self._mode(code), cf.WRAP_LIB)

    def test_public_extension_wraps_as_lib(self):
        # §26.1 defaults extensions to internal; the docs spell `public extension`.
        code = "public extension ReadOnlySpan<byte>\n  public method F(): int => 0;\nend;\n"
        self.assertEqual(self._mode(code), cf.WRAP_LIB)

    def test_pure_method_wraps_as_lib(self):
        code = "public static pure method Clamp(x: int): int\n  return x;\nend Clamp;\n"
        mode, wrapped = cf.classify_wrap(code)
        self.assertEqual(mode, cf.WRAP_LIB)
        self.assertTrue(wrapped.startswith("module Conf;\nclass W\n"))

    def test_import_wraps_as_stmt(self):
        code = "import Bezel.Collections;\nvar xs := new List<int>();\n"
        mode, wrapped = cf.classify_wrap(code)
        self.assertEqual(mode, cf.WRAP_STMT)
        self.assertIn("method Main()", wrapped)

    def test_import_then_statements_lands_in_main(self):
        # v0.247 regeneration: the leading import block is hoisted and the
        # remainder dispatches on its own shape — statements go INSIDE Main
        # (the old template stranded them at module level).
        code = "import System.Collections.Linq;\nvar xs := (1..3).ToArray();\n"
        mode, wrapped = cf.classify_wrap(code)
        self.assertEqual(mode, cf.WRAP_STMT)
        self.assertIn("    var xs := (1..3).ToArray();", wrapped)
        self.assertLess(wrapped.index("import System.Collections.Linq;"),
                        wrapped.index("class W"))

    def test_import_then_declaration_stays_lib(self):
        code = "import System.Interop;\npublic record R(Raw: IntPtr);\n"
        mode, wrapped = cf.classify_wrap(code)
        self.assertEqual(mode, cf.WRAP_LIB)
        self.assertLess(wrapped.index("module Conf;"),
                        wrapped.index("import System.Interop;"))
        self.assertLess(wrapped.index("import System.Interop;"),
                        wrapped.index("public record R"))

    def test_method_declaration_wraps_as_lib(self):
        code = "public method Foo(x: int) => x + 1;\n"
        mode, wrapped = cf.classify_wrap(code)
        self.assertEqual(mode, cf.WRAP_LIB)
        self.assertTrue(wrapped.startswith("module Conf;\nclass W\n"))

    def test_field_declaration_wraps_as_lib(self):
        code = "private _count: int;\n"
        mode, wrapped = cf.classify_wrap(code)
        self.assertEqual(mode, cf.WRAP_LIB)
        # Field wrapper adds a Main so the class has an entry point scaffold.
        self.assertIn("method Main()", wrapped)

    def test_default_statements_indent_in_main(self):
        code = "var x := 1;\nConsole.WriteLine(x);\n"
        mode, wrapped = cf.classify_wrap(code)
        self.assertEqual(mode, cf.WRAP_STMT)
        # sed 's/^/    /' — every body line indented four spaces.
        self.assertIn("    var x := 1;", wrapped)
        self.assertIn("  end Main;", wrapped)

    def test_default_indents_every_line_including_blank(self):
        # A trailing newline must not yield a phantom 5th-space line; matches sed.
        wrapped = cf.classify_wrap("var x := 1;\n")[1]
        self.assertIn("    var x := 1;\n", wrapped)

    def test_as_is_source_is_returned_unchanged(self):
        code = "module X;\nclass C\n  method Main()\n  end Main;\nend C;\n"
        _, wrapped = cf.classify_wrap(code)
        self.assertEqual(wrapped, code)


# -----------------------------------------------------------------------
# splice_preamble — the awk splice for notes=preamble.
# -----------------------------------------------------------------------

class SplicePreambleTest(unittest.TestCase):
    def test_inserts_after_first_module_line(self):
        # Blank lines directly after `module` are hopped with the import block
        # (v0.247 regeneration): the stub still precedes every TopDecl.
        wrapped = "module Conf;\n\nclass W\nend W;\n"
        out = cf.splice_preamble(wrapped, "record Point(X: int);\n")
        self.assertEqual(out, "module Conf;\n\nrecord Point(X: int);\nclass W\nend W;\n")

    def test_inserts_after_leading_imports(self):
        # SourceFile ::= ModuleDecl Import* TopDecl* (spec A.1): the stub must
        # come after the imports, or they would be ill-placed.
        wrapped = "module Conf;\nimport System.Interop;\nclass W\nend W;\n"
        out = cf.splice_preamble(wrapped, "record Point(X: int);\n")
        self.assertEqual(out, "module Conf;\nimport System.Interop;\n"
                              "record Point(X: int);\nclass W\nend W;\n")

    def test_supplies_missing_trailing_newline(self):
        # awk getline+print line-terminates every preamble line.
        out = cf.splice_preamble("module Conf;\nbody\n", "stub A")
        self.assertEqual(out, "module Conf;\nstub A\nbody\n")

    def test_no_module_line_means_no_splice(self):
        # Every wrap template emits `module Conf;`, so this is defensive parity
        # with awk (which would simply never match the trigger).
        out = cf.splice_preamble("no module here\n", "STUB\n")
        self.assertEqual(out, "no module here\n")


# -----------------------------------------------------------------------
# is_crash + evaluate_run — the run-phase decision tree.
# -----------------------------------------------------------------------

class IsCrashTest(unittest.TestCase):
    def test_signal_codes_are_crashes(self):
        for rc in (134, 137, 139, 128, 255):
            self.assertTrue(cf.is_crash(rc), rc)

    def test_exit_70_unhandled_exception_is_not_a_crash(self):
        # The runtime's unhandled-exception exit is acceptable for `compiles`.
        self.assertFalse(cf.is_crash(70))

    def test_zero_and_normal_are_not_crashes(self):
        for rc in (0, 1, 2, 70, 100, 127):
            self.assertFalse(cf.is_crash(rc), rc)


class EvaluateRunTest(unittest.TestCase):
    def test_compiles_pass_on_clean_exit(self):
        ok, kind, detail = cf.evaluate_run("compiles", 0, "")
        self.assertTrue(ok)
        self.assertEqual((kind, detail), ("", ""))

    def test_compiles_pass_on_exit_70(self):
        ok, _, _ = cf.evaluate_run("compiles", 70, "boom")
        self.assertTrue(ok)

    def test_compiles_fail_on_crash(self):
        ok, kind, detail = cf.evaluate_run("compiles", 139, "")
        self.assertFalse(ok)
        self.assertEqual(kind, "crash/signal")
        self.assertIn("139", detail)

    def test_run_match_first_line(self):
        ok, _, _ = cf.evaluate_run("run:hello", 0, "hello\nworld\n")
        self.assertTrue(ok)

    def test_run_strips_cr_before_compare(self):
        ok, _, _ = cf.evaluate_run("run:hello", 0, "hello\r\n")
        self.assertTrue(ok)

    def test_run_mismatch(self):
        ok, kind, detail = cf.evaluate_run("run:hello", 0, "bye\n")
        self.assertFalse(ok)
        self.assertEqual(kind, "wrong-result")
        self.assertIn("hello", detail)
        self.assertIn("bye", detail)

    def test_rc_match(self):
        ok, _, _ = cf.evaluate_run("rc:3", 3, "")
        self.assertTrue(ok)

    def test_rc_mismatch(self):
        ok, kind, detail = cf.evaluate_run("rc:3", 0, "")
        self.assertFalse(ok)
        self.assertEqual(kind, "wrong-result")
        self.assertIn("3", detail)

    def test_unknown_expectation_passes_when_no_crash(self):
        ok, _, _ = cf.evaluate_run("n/a", 0, "")
        self.assertTrue(ok)

    def test_unknown_expectation_fails_on_crash(self):
        ok, kind, _ = cf.evaluate_run("n/a", 134, "")
        self.assertFalse(ok)
        self.assertEqual(kind, "crash/signal")


# -----------------------------------------------------------------------
# read_manifest — faithful TSV split.
# -----------------------------------------------------------------------

class ReadManifestTest(unittest.TestCase):
    def _write(self, text):
        fd, path = tempfile.mkstemp(suffix=".tsv")
        with os.fdopen(fd, "w") as f:
            f.write(text)
        self.addCleanup(os.remove, path)
        return path

    def test_skips_header_and_blanks(self):
        path = self._write(
            "case-id\tdoc\tsection\tclass\texpectation\tnotes\n"
            "\n"
            "spec_0001\tspec\tS\tfragment\tcompiles\t\n")
        rows = cf.read_manifest(path)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["case_id"], "spec_0001")
        self.assertEqual(rows[0]["notes"], "")

    def test_pads_short_rows(self):
        # A row missing the notes column still yields an empty notes field.
        path = self._write("id\tdoc\tsec\tfragment\tcompiles\n")
        rows = cf.read_manifest(path)
        self.assertEqual(rows[0]["notes"], "")
        self.assertEqual(rows[0]["expectation"], "compiles")

    def test_no_tab_in_value_is_not_quote_processed(self):
        # A literal double-quote must survive (TSV, not CSV — bash read semantics).
        path = self._write('id\td\t"s\tfragment\tcompiles\t\n')
        rows = cf.read_manifest(path)
        # The quote lands in the 3rd column (section), unprocessed.
        self.assertEqual(rows[0]["doc"], "d")
        self.assertEqual(rows[0]["section"], '"s')


# -----------------------------------------------------------------------
# _format_line — output parity with the bash harness.
# -----------------------------------------------------------------------

class FormatLineTest(unittest.TestCase):
    def test_pass_no_detail(self):
        self.assertEqual(cf._format_line("PASS", "spec_0001", "", ""),
                         "PASS spec_0001")

    def test_pass_with_detail_suffix(self):
        self.assertEqual(cf._format_line("PASS", "spec_0001", "", "[lib-only; no run check]"),
                         "PASS spec_0001 [lib-only; no run check]")

    def test_fail_category_and_detail(self):
        self.assertEqual(cf._format_line("FAIL", "spec_0001", "compile-error", "syntax boom"),
                         "FAIL spec_0001 [compile-error] syntax boom")


# -----------------------------------------------------------------------
# run_case decision tree — driven through an injected fake `run`.
# -----------------------------------------------------------------------

class RunCaseTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(self._cleanup)
        self.cases = os.path.join(self.tmp, "cases")
        os.makedirs(self.cases)
        # A fake bezelc marker that satisfies the X_OK check in main(); run_case
        # itself never checks it (it only calls `run`).
        self.bezelc = os.path.join(self.tmp, "fake_bezelc")
        with open(self.bezelc, "w") as f:
            f.write("#!/bin/sh\nexit 0\n")
        os.chmod(self.bezelc, stat.S_IRWXU)

    def _cleanup(self):
        import shutil
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _case(self, name, code, cls="fragment", expectation="compiles", notes=""):
        with open(os.path.join(self.cases, name + ".bzl"), "w") as f:
            f.write(code)
        return {"case_id": name, "doc": "d", "section": "S", "cls": cls,
                "expectation": expectation, "notes": notes}

    def _run_factory(self, compile_rc=0, compile_log="", run_rc=0, run_log=""):
        """A fake `run` that returns canned results keyed on whether the command
        is a compile (argv[0] == bezelc) or an exec (argv[0] == the out exe).
        On a successful compile it materializes the `-o <out>` artifact as an
        executable so run_case's runnability check reaches the run phase (the
        real bezelc writes that file; the fake must too)."""
        calls = []

        def fake(cmd, cwd=None):
            calls.append(list(cmd))
            if cmd[0] == self.bezelc:
                if compile_rc == 0 and "-o" in cmd:
                    out = cmd[cmd.index("-o") + 1]
                    with open(out, "w") as f:
                        f.write("")
                    os.chmod(out, stat.S_IRWXU)
                return compile_rc, compile_log
            return run_rc, run_log

        fake.calls = calls
        return fake

    def test_error_case_passes_when_compile_fails(self):
        case = self._case("e1", "module Conf;\nclass W\nend W;\n", cls="error")
        run = self._run_factory(compile_rc=1, compile_log="err")
        status, kind, detail = cf.run_case(case, self.bezelc, self.cases,
                                           self.tmp, None, run=run)
        self.assertEqual((status, kind), ("PASS", ""))

    def test_error_case_fails_missing_diagnostic_when_compile_succeeds(self):
        case = self._case("e2", "module Conf;\nclass W\nend W;\n", cls="error")
        run = self._run_factory(compile_rc=0, compile_log="")
        status, kind, detail = cf.run_case(case, self.bezelc, self.cases,
                                           self.tmp, None, run=run)
        self.assertEqual(status, "FAIL")
        self.assertEqual(kind, "missing-diagnostic")

    def test_compile_timeout_beats_error_class(self):
        # rc==124 (timeout) is checked BEFORE the error-class branch; a timeout
        # on an `error` case is a timeout, not a pass.
        case = self._case("e3", "module Conf;\nclass W\nend W;\n", cls="error")
        run = self._run_factory(compile_rc=124)
        status, kind, detail = cf.run_case(case, self.bezelc, self.cases,
                                           self.tmp, None, run=run)
        self.assertEqual((status, kind, detail), ("FAIL", "timeout", "compile timed out"))

    def test_fragment_compile_error(self):
        case = self._case("f1", "module Conf;\nclass W\nend W;\n")
        run = self._run_factory(compile_rc=1, compile_log="syntax error here")
        status, kind, detail = cf.run_case(case, self.bezelc, self.cases,
                                           self.tmp, None, run=run)
        self.assertEqual(status, "FAIL")
        self.assertEqual(kind, "compile-error")

    def test_lib_mode_passes_without_running(self):
        # A type-decl fragment compiles as a library; success => PASS, no run.
        case = self._case("f2", "public record Point(X: int);\n")
        run = self._run_factory(compile_rc=0)
        status, kind, detail = cf.run_case(case, self.bezelc, self.cases,
                                           self.tmp, None, run=run)
        self.assertEqual(status, "PASS")
        # compiles expectation → no lib-only suffix.
        self.assertEqual(detail, "")
        # The library compile uses -c; the runner must not have run an exe.
        self.assertEqual(len(run.calls), 1)

    def test_stmt_run_match(self):
        case = self._case("f3", "Console.WriteLine(\"hi\");\n", expectation="run:hi")
        run = self._run_factory(compile_rc=0, run_rc=0, run_log="hi\n")
        status, kind, detail = cf.run_case(case, self.bezelc, self.cases,
                                           self.tmp, None, run=run)
        self.assertEqual((status, kind, detail), ("PASS", "", ""))

    def test_stmt_run_crash(self):
        case = self._case("f4", "var x := 1;\n")
        run = self._run_factory(compile_rc=0, run_rc=139)
        status, kind, detail = cf.run_case(case, self.bezelc, self.cases,
                                           self.tmp, None, run=run)
        self.assertEqual(kind, "crash/signal")
        self.assertEqual(status, "FAIL")

    def test_run_timeout(self):
        case = self._case("f5", "var x := 1;\n")
        run = self._run_factory(compile_rc=0, run_rc=124)
        status, kind, detail = cf.run_case(case, self.bezelc, self.cases,
                                           self.tmp, None, run=run)
        self.assertEqual((status, kind, detail), ("FAIL", "timeout", "run timed out"))

    def test_preamble_spliced_when_notes_preamble(self):
        case = self._case("f6", "public record Point(X: int);\n", notes="preamble")
        captured = {}

        def fake(cmd, cwd=None):
            # Capture the wrapped source the compile was invoked on.
            if cmd[0] == self.bezelc:
                with open(cmd[-1]) as f:
                    captured["src"] = f.read()
                return 0, ""
            return 0, ""

        cf.run_case(case, self.bezelc, self.cases, self.tmp,
                    "record Point(X: int);\n", run=fake)
        # The preamble stub appears after `module Conf;` (and any leading
        # imports/blank lines), before the first TopDecl.
        self.assertIn("record Point(X: int);", captured["src"])
        self.assertLess(captured["src"].index("module Conf;"),
                        captured["src"].index("record Point(X: int);"))

    def test_missing_source_file(self):
        case = {"case_id": "nope", "doc": "", "section": "", "cls": "fragment",
                "expectation": "compiles", "notes": ""}
        status, kind, detail = cf.run_case(case, self.bezelc, self.cases,
                                           self.tmp, None, run=self._run_factory())
        self.assertEqual((status, kind, detail),
                         ("FAIL", "compile-error", "source file missing"))


# -----------------------------------------------------------------------
# main() — the PASS>0 silent-green guard (Plan-A 5.1) + missing-bezelc.
# -----------------------------------------------------------------------

class MainGuardTest(unittest.TestCase):
    """The load-bearing Plan-A 5.1 test: a sweep that validated NOTHING must
    exit 1, not 0. Built on a synthetic base_dir + fake run so it never touches
    the real manifest or bezelc."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(self._cleanup)
        os.makedirs(os.path.join(self.tmp, "cases"))
        self.bezelc = os.path.join(self.tmp, "fake_bezelc")
        with open(self.bezelc, "w") as f:
            f.write("#!/bin/sh\nexit 0\n")
        os.chmod(self.bezelc, stat.S_IRWXU)

    def _cleanup(self):
        import shutil
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _manifest(self, text):
        with open(os.path.join(self.tmp, "manifest.tsv"), "w") as f:
            f.write(text)

    def _capture(self, *args, **kw):
        out = io.StringIO()
        err = io.StringIO()
        rc = [None]

        def go():
            with __import__("contextlib").redirect_stdout(out), \
                 __import__("contextlib").redirect_stderr(err):
                rc[0] = cf.main(*args, **kw)
        go()
        return rc[0], out.getvalue(), err.getvalue()

    def _ok_run(self, cmd, cwd=None):
        # Everything "compiles" cleanly and exits 0.
        return 0, ""

    def test_pass_gt_zero_guard_all_skip(self):
        # Plan-A 5.1: every case `skip` ⇒ PASS=0, FAIL=0. The OLD bash exit 0;
        # the guard must FAIL LOUD.
        self._manifest("case-id\tdoc\tsection\tclass\texpectation\tnotes\n"
                       "a\td\ts\tskip\tn/a\tskip:x\n")
        rc, out, err = self._capture([self.bezelc], base_dir=self.tmp,
                                     run=self._ok_run)
        self.assertEqual(rc, 1)
        self.assertIn("PASS=0", err)
        self.assertIn("Plan-A 5.1", err)

    def test_pass_gt_zero_guard_empty_manifest(self):
        # Only a header row ⇒ nothing to validate ⇒ guard fires.
        self._manifest("case-id\tdoc\tsection\tclass\texpectation\tnotes\n")
        rc, out, err = self._capture([self.bezelc], base_dir=self.tmp,
                                     run=self._ok_run)
        self.assertEqual(rc, 1)
        self.assertIn("PASS=0", err)

    def test_guard_does_not_fire_when_at_least_one_passes(self):
        self._manifest("case-id\tdoc\tsection\tclass\texpectation\tnotes\n"
                       "a\td\ts\tfragment\tcompiles\t\n")
        with open(os.path.join(self.tmp, "cases", "a.bzl"), "w") as f:
            f.write("var x := 1;\n")
        rc, out, err = self._capture([self.bezelc], base_dir=self.tmp,
                                     run=self._ok_run)
        self.assertEqual(rc, 0)

    def test_fail_gt_zero_takes_precedence_over_pass_zero_message(self):
        # All-fail: FAIL>0 is the accurate path; the guard's "validated nothing"
        # wording must not mislabel it (both exit 1, but only FAIL>0 prints).
        self._manifest("case-id\tdoc\tsection\tclass\texpectation\tnotes\n"
                       "a\td\ts\tfragment\tcompiles\t\n")
        with open(os.path.join(self.tmp, "cases", "a.bzl"), "w") as f:
            f.write("var x := 1;\n")

        def fail_run(cmd, cwd=None):
            return 1, "syntax error"
        rc, out, err = self._capture([self.bezelc], base_dir=self.tmp,
                                     run=fail_run)
        self.assertEqual(rc, 1)
        self.assertNotIn("PASS=0", err)  # the guard wording is not the trigger
        self.assertIn("compile-error", out)

    def test_missing_bezelc_exits_1(self):
        self._manifest("case-id\tdoc\tsection\tclass\texpectation\tnotes\n")
        rc = cf.main([os.path.join(self.tmp, "nope")], base_dir=self.tmp,
                     run=self._ok_run)
        self.assertEqual(rc, 1)


if __name__ == "__main__":
    unittest.main()
