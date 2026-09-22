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

    # v0.242 (#3052): `closed` joined A.3's `Modifier` production. §11.7's
    # worked example leads with `public closed class Shape`, so a fragment
    # extracted from it must classify as a type declaration, not fall through
    # to the statement wrap (where it could never compile).
    def test_closed_class_declaration_wraps_as_lib(self):
        code = ("public closed class Shape\n"
                "  public abstract method Area(): double;\n"
                "end Shape;\n")
        mode, wrapped = cf.classify_wrap(code)
        self.assertEqual(mode, cf.WRAP_LIB)
        self.assertIn(code, wrapped)

    def test_closed_class_without_visibility_wraps_as_lib(self):
        self.assertEqual(self._mode("closed class Shape\nend Shape;\n"),
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
# The composite ("mixed") wrap — opt-in via the `wrap:mixed` manifest note.
# -----------------------------------------------------------------------

# -----------------------------------------------------------------------
# needs_async_main / main_header — the synthesized `Main` signature.
#
# spec §33.2 (v0.306) admits `public static async method Main(): Task` as one of
# the four accepted entry-point signatures, so promoting the synthesized Main
# for a body that awaits is enclosing scaffolding, not an edit of the document.
# -----------------------------------------------------------------------

class NeedsAsyncMainTest(unittest.TestCase):
    def test_await_statement_needs_async(self):
        self.assertTrue(cf.needs_async_main("var x := await FetchAsync(u);\n"))

    def test_await_using_needs_async(self):
        self.assertTrue(cf.needs_async_main("await using var c := await Open();\n"))

    def test_await_for_needs_async(self):
        self.assertTrue(cf.needs_async_main("await for var l in S() do end;\n"))

    def test_no_await_does_not(self):
        self.assertFalse(cf.needs_async_main("var x := 1;\nDoWork();\n"))

    def test_await_only_in_a_line_comment_does_not(self):
        # A prose comment mentioning the keyword must not change the wrap.
        self.assertFalse(cf.needs_async_main("var x := 1;   // then await it\n"))

    def test_identifier_containing_await_does_not(self):
        self.assertFalse(cf.needs_async_main("var awaited := 1;\nawaiter.Go();\n"))

    def test_member_named_await_does_not(self):
        self.assertFalse(cf.needs_async_main("x.await;\n"))

    def test_main_header_promotes_only_when_needed(self):
        self.assertEqual(cf.main_header("var x := 1;\n"),
                         "  public static method Main()\n")
        self.assertEqual(cf.main_header("var x := await F();\n"),
                         "  public static async method Main(): Task\n")


class AsyncMainWrapTest(unittest.TestCase):
    def test_stmt_wrap_promotes_main(self):
        mode, src = cf.classify_wrap("var x := await FetchAsync(u);\n")
        self.assertEqual(mode, cf.WRAP_STMT)
        self.assertIn("public static async method Main(): Task", src)
        # The document's own text is untouched: only the enclosing lines differ.
        self.assertIn("var x := await FetchAsync(u);", src)

    def test_stmt_wrap_leaves_plain_main_alone(self):
        _, src = cf.classify_wrap("var x := 1;\n")
        self.assertIn("public static method Main()", src)
        self.assertNotIn("async", src)

    def test_mixed_wrap_promotes_main(self):
        _, src = cf.wrap_mixed("record P(X: int);\nvar v := await F();\n")
        self.assertIn("public static async method Main(): Task", src)
        self.assertIn("record P(X: int);", src)

    def test_mixed_wrap_ignores_await_inside_a_member(self):
        # The `await` lives in a class-member item, which classify_top_level_item
        # sorts into `member` — the Main body has none, so no promotion.
        _, src = cf.wrap_mixed(
            "public async method M(): Task\n  await F();\nend M;\n"
            "var x := 1;\n")
        self.assertIn("public static method Main()", src)

    def test_env_local_still_anchors_on_a_promoted_main(self):
        # The `local` splice anchors on the Main declaration line; before the
        # promotion the anchor regex saw only the plain spelling, so an
        # async-promoted case would have failed with `env-error`.
        _, src = cf.classify_wrap("var y := await F(x);\n")
        entries = cf.parse_env("//@ local x\nvar x: int := 1;\n")
        out = cf.splice_env(src, ["x"], entries)
        self.assertIn("var x: int := 1;", out)
        self.assertIn("public static async method Main(): Task", out)


class SplitTopLevelItemsTest(unittest.TestCase):
    def test_split_is_lossless(self):
        # The scaffolding must never drop or rewrite a byte of the doc block.
        code = ("public class Logger\n  public method Log(n: int);\nend Logger;\n"
                "\nvar lg := new Logger();\nlg.Log(1);\n")
        self.assertEqual("".join(cf.split_top_level_items(code)), code)

    def test_block_decl_absorbs_its_column0_end(self):
        code = "public class A\n  x: int;\nend A;\nvar y := 1;\n"
        self.assertEqual(cf.split_top_level_items(code),
                         ["public class A\n  x: int;\nend A;\n", "var y := 1;\n"])

    def test_single_line_decl_is_its_own_item(self):
        code = "public record Point(X: int, Y: int);\nvar p := new Point(1, 2);\n"
        self.assertEqual(len(cf.split_top_level_items(code)), 2)

    def test_column0_continuation_keywords_stay_with_their_item(self):
        # `else`/`catch`/`finally` in column 0 continue the statement in
        # progress; only the absorbed `end` closes it.
        code = "if c then\n  a();\nelse\n  b();\nend;\nc();\n"
        self.assertEqual(cf.split_top_level_items(code),
                         ["if c then\n  a();\nelse\n  b();\nend;\n", "c();\n"])

    def test_leading_blank_and_comment_ride_with_the_next_item(self):
        code = "\n// note\npublic record P(X: int);\n"
        self.assertEqual(cf.split_top_level_items(code), [code])


class ClassifyTopLevelItemTest(unittest.TestCase):
    def test_kinds(self):
        cases = [
            ("import System.Text;\n", "import"),
            ("public class A\nend A;\n", "moddecl"),
            ("public record P(X: int);\n", "moddecl"),
            ("extension Array<T>\nend;\n", "moddecl"),
            ("public choice C\n  A;\nend C;\n", "moddecl"),
            ("public method Log(m: string)\nend Log;\n", "member"),
            ("private owned _log: FileStream;\n", "member"),
            ("var x := 1;\n", "stmt"),
            ("lg.Log(1);\n", "stmt"),
        ]
        for code, want in cases:
            self.assertEqual(cf.classify_top_level_item(code), want, code)

    def test_head_skips_leading_comments_and_blanks(self):
        self.assertEqual(
            cf.classify_top_level_item("\n// prose\npublic class A\nend A;\n"),
            "moddecl")


class WrapMixedTest(unittest.TestCase):
    def test_decls_and_statements_both_land_somewhere_legal(self):
        # The shape skip rule R7 (`skip:mixed`) declared unwrappable: a type
        # declaration plus top-level statements in one block.
        code = ("public class Logger\n  public method Log(n: int) end Log;\nend Logger;\n"
                "\nvar lg := new Logger();\nlg.Log(1);\n")
        mode, src = cf.wrap_mixed(code)
        self.assertEqual(mode, cf.WRAP_STMT)
        self.assertTrue(src.startswith("module Conf;\n"))
        # declaration is at module level, before `class W`
        self.assertLess(src.index("public class Logger"), src.index("class W\n"))
        # statements are inside Main
        self.assertLess(src.index("public static method Main()"), src.index("lg.Log(1);"))
        self.assertLess(src.index("lg.Log(1);"), src.index("end Main;"))

    def test_imports_precede_declarations(self):
        # SourceFile ::= ModuleDecl Import* TopDecl* (spec Appendix A.1).
        mode, src = cf.wrap_mixed("public record P(X: int);\nimport System.Text;\nvar p := 1;\n")
        self.assertLess(src.index("import System.Text;"), src.index("public record P"))
        self.assertLess(src.index("module Conf;"), src.index("import System.Text;"))

    def test_members_land_in_the_class_not_at_module_level(self):
        mode, src = cf.wrap_mixed("private _items: Array<int> := new();\nvar xs := 1;\n")
        self.assertLess(src.index("class W\n"), src.index("private _items"))
        self.assertLess(src.index("private _items"), src.index("public static method Main()"))

    def test_every_block_token_survives_the_wrap(self):
        # No token inside the block is rewritten or dropped — the corpus runs
        # the documents verbatim.
        code = ("public record P(X: int);\n"
                "public method M() end M;\n"
                "import System.Text;\n"
                "var v := new P(1);\n")
        _, src = cf.wrap_mixed(code)
        for line in code.splitlines():
            self.assertIn(line.strip(), src)


class EnvTest(unittest.TestCase):
    CAT = ("// header prose\n"
           "//@ local score\n"
           "// derivation: the range patterns force an integral.\n"
           "var score: int := 85;\n"
           "\n"
           "//@ member _name\n"
           "  private _name: string := \"\";\n"
           "\n"
           "//@ type Widget\n"
           "public record Widget(X: int);\n")

    def test_parse_env(self):
        e = cf.parse_env(self.CAT)
        self.assertEqual(sorted(e), ["Widget", "_name", "score"])
        self.assertEqual(e["score"][0], cf.ENV_LOCAL)
        self.assertEqual(e["_name"][0], cf.ENV_MEMBER)
        self.assertEqual(e["Widget"][0], cf.ENV_TYPE)
        # the derivation comment rides along into the emitted source
        self.assertIn("derivation:", e["score"][1])
        self.assertIn("var score: int := 85;", e["score"][1])

    def test_parse_env_ignores_prose_before_the_first_header(self):
        self.assertNotIn("header prose", "".join(
            b for _, b in cf.parse_env(self.CAT).values()))

    def test_env_names_ordered_and_deduped(self):
        self.assertEqual(cf.env_names("wrap:mixed; env:score,month,score"),
                         ["month", "score"])
        self.assertEqual(cf.env_names("preamble"), [])
        self.assertEqual(cf.env_names(""), [])

    def test_splice_env_places_each_kind_at_its_anchor(self):
        _, wrapped = cf.wrap_mixed("var g := score;\n")
        out = cf.splice_env(wrapped, ["Widget", "_name", "score"],
                            cf.parse_env(self.CAT))
        # type beside the module's other top-level decls, before `class W`
        self.assertLess(out.index("public record Widget"), out.index("class W\n"))
        # member inside class W, before Main
        self.assertLess(out.index("class W\n"), out.index("private _name"))
        self.assertLess(out.index("private _name"),
                        out.index("public static method Main()"))
        # local at the top of the Main body, ahead of the fragment's statements
        self.assertLess(out.index("public static method Main()"),
                        out.index("var score: int := 85;"))
        self.assertLess(out.index("var score: int := 85;"), out.index("var g := score;"))

    def test_unknown_env_name_raises(self):
        _, wrapped = cf.wrap_mixed("var g := 1;\n")
        with self.assertRaises(cf.EnvError):
            cf.splice_env(wrapped, ["nope"], cf.parse_env(self.CAT))

    def test_missing_anchor_raises_rather_than_silently_dropping(self):
        # A silently-unapplied ambient would make its row pass for a reason
        # nobody chose. The lib wrap has a `class W` but no `Main`.
        wrapped = "module Conf;\nclass W\n  method F() end F;\nend W;\n"
        with self.assertRaises(cf.EnvError):
            cf.splice_env(wrapped, ["score"], cf.parse_env(self.CAT))

    def test_run_case_reports_env_error_as_a_failure(self):
        # An env misconfiguration must be a loud FAIL, never a quiet pass.
        tmp = tempfile.mkdtemp()
        self.addCleanup(lambda: __import__("shutil").rmtree(tmp, ignore_errors=True))
        cases = os.path.join(tmp, "cases")
        os.makedirs(cases)
        with open(os.path.join(cases, "z.bzl"), "w") as f:
            f.write("var g := 1;\n")
        case = {"case_id": "z", "doc": "d", "section": "S", "cls": "fragment",
                "expectation": "compiles", "notes": "wrap:mixed; env:absent"}
        status, kind, detail = cf.run_case(
            case, "/bin/true", cases, tmp, None,
            run=lambda cmd, cwd=None: (0, ""), env_entries={})
        self.assertEqual(status, "FAIL")
        self.assertEqual(kind, "env-error")


class NoteTokensTest(unittest.TestCase):
    def test_semicolon_separated_tokens(self):
        self.assertEqual(cf.note_tokens("wrap:mixed; preamble"),
                         {"wrap:mixed", "preamble"})

    def test_prose_containing_a_keyword_is_not_that_token(self):
        # `name-collides-with-preamble (…)` must NOT switch the preamble splice
        # on — exact-token matching, not substring.
        self.assertNotIn("preamble",
                         cf.note_tokens("name-collides-with-preamble (Shape/Circle)"))

    def test_empty(self):
        self.assertEqual(cf.note_tokens(""), set())
        self.assertEqual(cf.note_tokens(None), set())


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


class SnippetTest(unittest.TestCase):
    """_snippet budgets characters over the WHOLE line, path included — a
    bezelc diagnostic opens with the absolute path of the file it compiled, so
    without `strip_dir` the surviving message shrinks as the checkout gets
    deeper (#4556)."""

    def test_without_strip_dir_a_deep_path_eats_the_whole_budget(self):
        deep = "/" + "/".join(["lane-worktree-segment"] * 6)
        log = deep + "/cases/s5_4_0001.bzl:20:23: no member named 'reallyLong'\n"
        snippet = cf._snippet(log, 120, 120)
        self.assertNotIn("no member named", snippet)

    def test_strip_dir_removes_the_prefix_before_the_budget_is_spent(self):
        # cut_chars/head_bytes match the rule-corpus runner's compile-error
        # tier (runner.py's `evaluate`, expect != reject/quiet branch).
        deep = "/" + "/".join(["lane-worktree-segment"] * 6) + "/cases"
        log = (deep + "/s5_4_0001.bzl:20:23: no member named 'reallyLongName' "
               "in type 'Widget'\n")
        snippet = cf._snippet(log, 120, 240, strip_dir=deep)
        self.assertNotIn(deep, snippet)
        self.assertIn("no member named 'reallyLongName'", snippet)

    def test_strip_dir_is_a_noop_when_the_prefix_is_absent(self):
        log = "s1_1_0001.bzl:1:1: boom\n"
        self.assertEqual(cf._snippet(log, 10, 20),
                         cf._snippet(log, 10, 20, strip_dir="/nonexistent"))

    def test_strip_dir_none_matches_prior_behavior(self):
        log = "/some/where/f.bzl:1:1: boom text\n"
        self.assertEqual(cf._snippet(log, 12, 30),
                         cf._snippet(log, 12, 30, strip_dir=None))


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

    def test_compile_error_detail_survives_a_deep_tmpdir(self):
        # #4556: bezelc compiles the case as <tmpdir>/<case_id>_wrapped.bzl and
        # its diagnostic opens with that absolute path. A lane worktree's
        # tmpdir prefix routinely runs 100+ characters; without run_case's
        # strip_dir=tmpdir it alone would consume the whole cut/head budget
        # and the real message ("no member named ...") never survives.
        deep_root = tempfile.mkdtemp()
        self.addCleanup(lambda: __import__("shutil").rmtree(
            deep_root, ignore_errors=True))
        deep_tmp = os.path.join(deep_root, *(["lane-worktree-segment"] * 5))
        os.makedirs(deep_tmp)
        case = self._case("f7", "module Conf;\nclass W\nend W;\n")

        def fake(cmd, cwd=None):
            if cmd[0] == self.bezelc:
                wrap_file = cmd[-1]
                return 1, (wrap_file + ":3:7: no member named "
                          "'reallyLongName' in type 'W'\n")
            return 0, ""

        status, kind, detail = cf.run_case(case, self.bezelc, self.cases,
                                           deep_tmp, None, run=fake)
        self.assertEqual((status, kind), ("FAIL", "compile-error"))
        self.assertIn("no member named 'reallyLongName'", detail)
        self.assertNotIn(deep_tmp, detail)

    def test_missing_diagnostic_detail_survives_a_deep_tmpdir(self):
        # Same property (#4556), the `cls == "error"` / compile-succeeded
        # branch: the compiler's own (successful) output can still carry the
        # wrapped file's absolute path, e.g. in an unrelated warning.
        deep_root = tempfile.mkdtemp()
        self.addCleanup(lambda: __import__("shutil").rmtree(
            deep_root, ignore_errors=True))
        deep_tmp = os.path.join(deep_root, *(["lane-worktree-segment"] * 5))
        os.makedirs(deep_tmp)
        case = self._case("f8", "module Conf;\nclass W\nend W;\n", cls="error")

        def fake(cmd, cwd=None):
            if cmd[0] == self.bezelc:
                wrap_file = cmd[-1]
                return 0, (wrap_file + ":1:1: warning: reallyLongDescriptive"
                          "UnusedImportWarning\n")
            return 0, ""

        status, kind, detail = cf.run_case(case, self.bezelc, self.cases,
                                           deep_tmp, None, run=fake)
        self.assertEqual((status, kind), ("FAIL", "missing-diagnostic"))
        self.assertIn("reallyLongDescriptiveUnusedImportWarning", detail)
        self.assertNotIn(deep_tmp, detail)


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
