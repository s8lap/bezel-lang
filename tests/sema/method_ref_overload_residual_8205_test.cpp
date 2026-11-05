// Spec-derived regression test for issue #8205 (residuals of #8121).
//
// SPEC §27.6 (v0.355), *Method references to overloaded methods*, verbatim:
//
//   "When the named method (or property) is overloaded, the reference is
//    resolved using the **target function type** at the use site."
//
//   "When no target type is available (e.g., `var f := Math.Abs;` with no
//    annotation), the reference is ambiguous and the compiler reports an error
//    pointing to the candidate overloads."
//
// SPEC §27.6 (v0.355), extension methods, verbatim:
//
//   "**Receiver-qualified** — `v.Scaled` over `extension Vec`, the member
//    resolved on the value exactly as the call `v.Scaled()` is (§26.5) …"
//
// and §26.5 rules 2/4: a call consults every applicable extension block, so
// overloads split across two blocks are ONE overload set.
//
// THE DEFECTS (measured on this fix's base):
//   A. `Console.WriteLine(v.Len)` over an OVERLOADED `Len` compiled and
//      printed `() -> int`: the `Object` parameter supplies no target function
//      type, so the no-target ambiguity rule applies and was never run there.
//   B. overloads split across TWO extension blocks: `a.Len(5)` resolved but
//      `var g: (int) -> int := a.Len;` was refused (§5.1) because the
//      reference's candidate set was the one block the bare lookup stopped at.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

bool hasError(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

const char kNoTarget[] =
    "the method reference `Len` is overloaded and has no target type to "
    "select by (\xC2\xA7""27.6 v0.307)";

const char kClassV[] =
    "class V\n"
    "  public constructor() end constructor;\n"
    "  public method Len(): int => 42;\n"
    "  public method Len(x: int): int => x + 100;\n"
    "end V;\n";

std::string classMain(const std::string& body) {
    return std::string("module M;\n") + kClassV +
           "class C\n"
           "  public static method Main(): int\n"
           "    var v := new V();\n"
           "    " + body + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

const char kSplitBlocks[] =
    "extension Array<int>\n"
    "  public method Len(): int => 1;\n"
    "end;\n"
    "extension IEnumerable<int>\n"
    "  public method Len(x: int): int => x + 200;\n"
    "end;\n";

std::string splitMain(const std::string& body) {
    return std::string("module M;\n") + kSplitBlocks +
           "class C\n"
           "  public static method Main(): int\n"
           "    var a: Array<int> := [1,2,3];\n"
           "    " + body + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

void expectRejected(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, needle));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

void expectCheckedStdout(const std::string& src, const std::string& out) {
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

}  // namespace

// ── A ───────────────────────────────────────────────────────────────────────

BEZEL_TEST(sema_method_ref_overload_residual_8205,
           overloaded_reference_to_console_writeline_is_ambiguous) {
    // per §27.6 (v0.355): the issue's literal repro
    expectRejected(classMain("Console.WriteLine(v.Len);"), kNoTarget);
}

BEZEL_TEST(sema_method_ref_overload_residual_8205,
           overloaded_extension_reference_to_console_writeline_is_ambiguous) {
    // per §27.6 (v0.355): the extension form of the same repro
    expectRejected(
        std::string("module M;\n"
                    "class V\n"
                    "  public constructor() end constructor;\n"
                    "end V;\n"
                    "extension V\n"
                    "  public method Len(): int => 42;\n"
                    "  public method Len(x: int): int => x + 100;\n"
                    "end;\n"
                    "class C\n"
                    "  public static method Main(): int\n"
                    "    var v := new V();\n"
                    "    Console.WriteLine(v.Len);\n"
                    "    return 0;\n"
                    "  end Main;\n"
                    "end C;\n"),
        kNoTarget);
}

BEZEL_TEST(sema_method_ref_overload_residual_8205,
           non_overloaded_reference_to_console_writeline_still_prints_type) {
    // control: a single candidate is not ambiguous (§27.6, §8.5)
    expectCheckedStdout(
        "module M;\n"
        "class V\n"
        "  public constructor() end constructor;\n"
        "  public method Len(): int => 42;\n"
        "end V;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var v := new V();\n"
        "    Console.WriteLine(v.Len);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "() -> int\n");
}

BEZEL_TEST(sema_method_ref_overload_residual_8205,
           overloaded_reference_with_a_target_still_selects) {
    // control: the target-typed selection is untouched
    expectCheckedStdout(classMain("var g: (int) -> int := v.Len;\n"
                                  "    Console.WriteLine(g(5));"),
                        "105\n");
}

// ── B ───────────────────────────────────────────────────────────────────────

BEZEL_TEST(sema_method_ref_overload_residual_8205,
           overloads_split_across_two_extension_blocks_resolve_by_target) {
    // per §27.6 + §26.5 (v0.355): the issue's literal repro
    expectCheckedStdout(splitMain("Console.WriteLine(a.Len(5));\n"
                                  "    var g: (int) -> int := a.Len;\n"
                                  "    Console.WriteLine(g(5));"),
                        "205\n205\n");
}

BEZEL_TEST(sema_method_ref_overload_residual_8205,
           split_blocks_reference_selects_the_other_block_by_target) {
    // the zero-arity member (the more specific block) is still selectable
    expectCheckedStdout(splitMain("var f: () -> int := a.Len;\n"
                                  "    Console.WriteLine(f());"),
                        "1\n");
}

BEZEL_TEST(sema_method_ref_overload_residual_8205,
           split_blocks_reference_without_a_target_is_ambiguous) {
    // per §27.6: the union is overloaded, so no target means ambiguous
    expectRejected(splitMain("var f := a.Len;"), kNoTarget);
}

BEZEL_TEST(sema_method_ref_overload_residual_8205,
           single_block_overload_set_still_resolves_by_target) {
    // control: one block, as #8121 pinned
    expectCheckedStdout(
        "module M;\n"
        "extension Array<int>\n"
        "  public method Len(): int => 1;\n"
        "  public method Len(x: int): int => x + 200;\n"
        "end;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var a: Array<int> := [1,2,3];\n"
        "    var g: (int) -> int := a.Len;\n"
        "    Console.WriteLine(g(5));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "205\n");
}

// ── F1 (review of 5d3a8ec0): a rule-4 tie in the split set ─────────────────
//
// SPEC §27.6 (v0.355): the receiver-qualified reference is "the member
// resolved on the value exactly as the call `v.Scaled()` is (§26.5)". SPEC
// §26.5 rule 4: two or more equally specific applicable blocks are a compile
// error. The call `a.Len(5)` below is that error, so the reference that the
// target `(int) -> int` resolves to the same arity is too — and it must not
// bind by declaration order (before this fix: 205 in one order, 305 in the
// other).

namespace {

const char kTieAmbiguity[] =
    "ambiguous extension member `Len`: 2 extensions are equally specific";

std::string tieMain(bool intFirst, const std::string& body) {
    const std::string intBlock =
        "extension IEnumerable<int>\n"
        "  public method Len(x: int): int => x + 200;\n"
        "end;\n";
    const std::string genBlock =
        "extension IEnumerable<T>\n"
        "  public method Len(x: int): int => x + 300;\n"
        "end;\n";
    return std::string("module M;\n"
                       "extension Array<int>\n"
                       "  public method Len(): int => 1;\n"
                       "end;\n") +
           (intFirst ? intBlock + genBlock : genBlock + intBlock) +
           "class C\n"
           "  public static method Main(): int\n"
           "    var a: Array<int> := [1,2,3];\n"
           "    " + body + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// The error is reported at the reference's own position (line 14 of
// tieMain). runAot carries the driver's stderr as the message, so the
// position is read from its `file:line:column:` prefix.
bool hasErrorAt(const JitResult& r, const std::string& needle, unsigned line,
                unsigned column) {
    const std::string located = ":" + std::to_string(line) + ":" +
                                std::to_string(column) + ": " + needle;
    return hasError(r, located);
}

void expectTieRejected(bool intFirst) {
    auto r = runAot(tieMain(intFirst, "var g: (int) -> int := a.Len;\n"
                                      "    Console.WriteLine(g(5));"));
    BEZEL_EXPECT_FALSE(r.ran);
    // `    var g: (int) -> int := ` is 27 columns: the reference `a.Len`
    BEZEL_EXPECT_TRUE(hasErrorAt(r, kTieAmbiguity, 14, 28));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

}  // namespace

BEZEL_TEST(sema_method_ref_overload_residual_8205,
           split_set_tie_at_the_selected_arity_is_ambiguous_int_block_first) {
    // per §27.6 + §26.5 rule 4 (v0.355): review probe p12
    expectTieRejected(true);
}

BEZEL_TEST(sema_method_ref_overload_residual_8205,
           split_set_tie_at_the_selected_arity_is_ambiguous_generic_block_first) {
    // per §27.6 + §26.5 rule 4 (v0.355): review probe p25 (blocks reversed)
    expectTieRejected(false);
}

BEZEL_TEST(sema_method_ref_overload_residual_8205,
           split_set_tie_the_call_of_that_arity_is_the_same_ambiguity) {
    // control (review probe p12b): the call the reference resolves as
    auto r = runAot(tieMain(true, "Console.WriteLine(a.Len(5));"));
    BEZEL_EXPECT_FALSE(r.ran);
    // `    Console.WriteLine(` is 22 columns: the call `a.Len(5)`
    BEZEL_EXPECT_TRUE(hasErrorAt(r, kTieAmbiguity, 14, 23));
}

BEZEL_TEST(sema_method_ref_overload_residual_8205,
           split_set_tie_at_another_arity_does_not_reject_the_selected_one) {
    // control: the target `() -> int` selects the Array<int> member; the tie
    // is at arity 1 only, as the call `a.Len()` is unambiguous (§26.5)
    expectCheckedStdout(tieMain(true, "var f: () -> int := a.Len;\n"
                                      "    Console.WriteLine(f());"),
                        "1\n");
}

BEZEL_TEST(sema_method_ref_overload_residual_8205,
           split_set_more_specific_block_wins_at_the_selected_arity) {
    // control (review probe p13): §26.5 rule 3, the Array<int> block's
    // `Len(x: int)` is more specific than IEnumerable<int>'s
    expectCheckedStdout(
        "module M;\n"
        "extension Array<int>\n"
        "  public method Len(): int => 1;\n"
        "  public method Len(x: int): int => x + 100;\n"
        "end;\n"
        "extension IEnumerable<int>\n"
        "  public method Len(x: int): int => x + 200;\n"
        "end;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var a: Array<int> := [1,2,3];\n"
        "    Console.WriteLine(a.Len(5));\n"
        "    var g: (int) -> int := a.Len;\n"
        "    Console.WriteLine(g(5));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "105\n105\n");
}

BEZEL_TEST(sema_method_ref_overload_residual_8205,
           split_set_generic_owner_block_and_concrete_foreign_block) {
    // control (review probe p11): a generic `extension Array<T>` owner and a
    // concrete foreign block, each selected by its own target
    expectCheckedStdout(
        "module M;\n"
        "extension Array<T>\n"
        "  public method Len(): int => 1;\n"
        "end;\n"
        "extension IEnumerable<int>\n"
        "  public method Len(x: int): int => x + 200;\n"
        "end;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var a: Array<int> := [1,2,3];\n"
        "    var g: (int) -> int := a.Len;\n"
        "    Console.WriteLine(g(5));\n"
        "    var f: () -> int := a.Len;\n"
        "    Console.WriteLine(f());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "205\n1\n");
}

// ── F2 (review of 7584c270): the §14.5 list rule decides the candidates ─────
//
// SPEC §27.6 (v0.355), *References to generic methods*: "Overload selection
// follows §14.5's list rule: with a list, only candidates declaring that many
// type parameters of their own take part ...; without one, only candidates
// declaring none take part". `v.Len` over `Len(): int` and `Len<T>(x: T): int`
// therefore has ONE candidate and no ambiguity.

namespace {

std::string genericMain(const std::string& body) {
    return std::string("module M;\n"
                       "class V\n"
                       "  public constructor() end constructor;\n"
                       "  public method Len(): int => 42;\n"
                       "  public method Len<T>(x: T): int => 7;\n"
                       "end V;\n"
                       "class C\n"
                       "  public static method Main(): int\n"
                       "    var v := new V();\n"
                       "    ") +
           body + "\n"
                  "    return 0;\n"
                  "  end Main;\n"
                  "end C;\n";
}

}  // namespace

BEZEL_TEST(sema_method_ref_overload_residual_8205,
           generic_overload_does_not_take_part_without_a_list_writeline) {
    // per §27.6 / §14.5 (v0.355): review probe p04
    expectCheckedStdout(genericMain("Console.WriteLine(v.Len);"),
                        "() -> int\n");
}

BEZEL_TEST(sema_method_ref_overload_residual_8205,
           generic_overload_does_not_take_part_without_a_list_var) {
    // per §27.6 / §14.5 (v0.355): review probe p23
    expectCheckedStdout(genericMain("var f := v.Len;\n"
                                    "    Console.WriteLine(f());"),
                        "42\n");
}

// ── F3: every other position with no target FUNCTION type ──────────────────
//
// SPEC §27.6 (v0.355): "When no target type is available (e.g., `var f :=
// Math.Abs;` with no annotation), the reference is ambiguous and the compiler
// reports an error pointing to the candidate overloads." A position declared
// `Object`, and an interpolation hole, supply no function type to select by.

BEZEL_TEST(sema_method_ref_overload_residual_8205,
           overloaded_reference_to_an_object_local_is_ambiguous) {
    // per §27.6 (v0.355): review probe p01
    expectRejected(classMain("var o: Object := v.Len;\n"
                             "    Console.WriteLine(o);"),
                   kNoTarget);
}

BEZEL_TEST(sema_method_ref_overload_residual_8205,
           overloaded_static_reference_to_a_user_object_parameter_is_ambiguous) {
    // per §27.6 (v0.355): review probe p22
    expectRejected(
        "module M;\n"
        "class V\n"
        "  public static method Len(): int => 42;\n"
        "  public static method Len(x: int): int => x + 100;\n"
        "end V;\n"
        "class U\n"
        "  public static method Show(o: Object): () => Console.WriteLine(o);\n"
        "end U;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    U.Show(V.Len);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        kNoTarget);
}

BEZEL_TEST(sema_method_ref_overload_residual_8205,
           overloaded_reference_in_an_interpolation_hole_is_ambiguous) {
    // per §27.6 / §18.5 (v0.355): review probe p08
    expectRejected(classMain("Console.WriteLine($\"{v.Len}\");"), kNoTarget);
}

BEZEL_TEST(sema_method_ref_overload_residual_8205,
           non_overloaded_reference_in_an_interpolation_hole_still_formats) {
    // control: one candidate, nothing to select (§27.6, §18.5)
    expectCheckedStdout(genericMain("Console.WriteLine($\"{v.Len}\");"),
                        "() -> int\n");
}

BEZEL_TEST(sema_method_ref_overload_residual_8205,
           overloaded_reference_with_a_function_target_still_selects) {
    // control: a target function type selects (§27.6)
    expectCheckedStdout(classMain("var f: () -> int := v.Len;\n"
                                  "    Console.WriteLine(f());"),
                        "42\n");
}

// ── D: the dedup record survives a speculative visit's rollback ─────────────
//
// The #8203 LINQ-operator argument peek (`o.Where(arg)` over a user
// `extension Object { Where(…) }`) visits `arg` once speculatively, rolls its
// diagnostics back, and visits it again in the normal order. The one-report-
// per-reference record (fix #8205) outlived the rollback, so the real visit
// reported nothing: a program the §27.6 no-target rule (or §26.5 rule 4)
// rejects compiled and ran (MEASURED, rev-8205b probes r21/r23/r24/r25 —
// verbatim below). Each control is the same program with `Where` renamed
// `Wherx` (no §5 operator name, so no peek). Every row also pins the
// record's purpose: the reference is diagnosed exactly ONCE.

namespace {

// rev-8205b r23_linq_peek_iife_var_infer.bzl
const char kPeekIifeVarInfer[] =
    "module M;\n"
    "class V\n"
    "  public static method Len(): int => 42;\n"
    "  public static method Len(x: int): int => x + 100;\n"
    "end V;\n"
    "extension Object\n"
    "  public method Where(n: int): int => n;\n"
    "end;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var o: Object := new Object();\n"
    "    Console.WriteLine(o.Where((lambda() => do var f := V.Len; return 1; end)()));\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// rev-8205b r25_linq_peek_iife_tie.bzl
const char kPeekIifeTie[] =
    "module M;\n"
    "extension Array<int>\n"
    "  public method Len(): int => 1;\n"
    "end;\n"
    "extension IEnumerable<int>\n"
    "  public method Len(x: int): int => x + 200;\n"
    "end;\n"
    "extension IEnumerable<T>\n"
    "  public method Len(x: int): int => x + 300;\n"
    "end;\n"
    "extension Object\n"
    "  public method Where(n: int): int => n;\n"
    "end;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var a: Array<int> := [1,2,3];\n"
    "    var o: Object := new Object();\n"
    "    Console.WriteLine(o.Where((lambda() => do var g: (int) -> int := a.Len; return g(5); end)()));\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// rev-8205b r21_linq_peek_interp_hole.bzl
const char kPeekInterpHole[] =
    "module M;\n"
    "class V\n"
    "  public static method Len(): int => 42;\n"
    "  public static method Len(x: int): int => x + 100;\n"
    "end V;\n"
    "extension Object\n"
    "  public method Where(s: string): string => s;\n"
    "end;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var o: Object := new Object();\n"
    "    Console.WriteLine(o.Where($\"{V.Len}\"));\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// rev-8205b r24_linq_peek_nested_object_param.bzl
const char kPeekNestedObjectParam[] =
    "module M;\n"
    "class V\n"
    "  public static method Len(): int => 42;\n"
    "  public static method Len(x: int): int => x + 100;\n"
    "end V;\n"
    "class U\n"
    "  public static method Show(x: Object): int => 3;\n"
    "end U;\n"
    "extension Object\n"
    "  public method Where(n: int): int => n;\n"
    "end;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var o: Object := new Object();\n"
    "    Console.WriteLine(o.Where(U.Show(V.Len)));\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// The control: every `Where` renamed `Wherx`.
std::string withoutPeek(std::string src) {
    for (std::size_t at = src.find("Where"); at != std::string::npos;
         at = src.find("Where", at + 5))
        src.replace(at, 5, "Wherx");
    return src;
}

std::size_t errorCount(const JitResult& r, const std::string& needle) {
    std::size_t n = 0;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        for (std::size_t at = d.message.find(needle); at != std::string::npos;
             at = d.message.find(needle, at + needle.size()))
            ++n;
    }
    return n;
}

void expectRejectedOnce(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_EQ(errorCount(r, needle), static_cast<std::size_t>(1));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

}  // namespace

BEZEL_TEST(sema_method_ref_overload_residual_8205,
           no_target_reference_inside_a_peeked_linq_argument_is_ambiguous) {
    // per §27.6 (v0.355): `var f := V.Len;` inside an immediately-invoked
    // lambda that the `Where` peek visits speculatively (r23; tip printed 1)
    expectRejectedOnce(kPeekIifeVarInfer, kNoTarget);
}

BEZEL_TEST(sema_method_ref_overload_residual_8205,
           no_target_reference_inside_a_non_linq_argument_is_ambiguous) {
    expectRejectedOnce(withoutPeek(kPeekIifeVarInfer), kNoTarget);
}

BEZEL_TEST(sema_method_ref_overload_residual_8205,
           rule_4_tie_inside_a_peeked_linq_argument_is_ambiguous) {
    // per §27.6 / §26.5 rule 4 (v0.355): the split-set tie at arity 1,
    // selected inside the peeked IIFE (r25; tip printed 205)
    expectRejectedOnce(kPeekIifeTie, kTieAmbiguity);
}

BEZEL_TEST(sema_method_ref_overload_residual_8205,
           rule_4_tie_inside_a_non_linq_argument_is_ambiguous) {
    expectRejectedOnce(withoutPeek(kPeekIifeTie), kTieAmbiguity);
}

BEZEL_TEST(sema_method_ref_overload_residual_8205,
           interpolation_hole_reference_in_a_peeked_linq_argument_is_ambiguous) {
    // per §27.6 / §18.5 (v0.355): `$"{V.Len}"` as the peeked argument (r21;
    // tip printed `() -> int`)
    expectRejectedOnce(kPeekInterpHole, kNoTarget);
}

BEZEL_TEST(sema_method_ref_overload_residual_8205,
           interpolation_hole_reference_in_a_non_linq_argument_is_ambiguous) {
    expectRejectedOnce(withoutPeek(kPeekInterpHole), kNoTarget);
}

BEZEL_TEST(sema_method_ref_overload_residual_8205,
           object_position_reference_in_a_peeked_linq_argument_is_ambiguous) {
    // per §27.6 (v0.355): `U.Show(V.Len)` at an `Object` parameter, nested in
    // the peeked argument (r24; tip printed 3)
    expectRejectedOnce(kPeekNestedObjectParam, kNoTarget);
}

BEZEL_TEST(sema_method_ref_overload_residual_8205,
           object_position_reference_in_a_non_linq_argument_is_ambiguous) {
    expectRejectedOnce(withoutPeek(kPeekNestedObjectParam), kNoTarget);
}
