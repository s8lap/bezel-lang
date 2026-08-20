// #2022 (§12.9 type-test invariance / §2.8.3 rule 4 monomorphization / §17.4 is/as,
// v0.197) — the runtime must mint a precise per-monomorphization descriptor
// for a constructed generic whose type ARGUMENT is a NULL-DECLARATION builtin
// generic (`Array<T>`, `Dictionary<K,V>`, `Task<T>`, … — declaration
// intentionally null per §2.5/§29.3), so that `is`/`as` distinguishes
// `Wrap<Array<int>>` from `Wrap<Array<string>>`.
//
// THE DEFECT. #2013 made appendDescKey recurse into the args of a null-decl
// builtin (so the descriptor/monomorphization KEY for Array<int> vs
// Array<string> differs). But the SEPARATE runtime gate `descTypeArgConcrete` /
// `descArgsConcrete` (LlvmBackend_Setup.cpp) — which decides whether a
// per-monomorphization descriptor is minted at all — still treated a null-decl
// `Named` arg as non-concrete, so `instanceDescriptorFor` (the `new`-site value
// descriptor) AND `perInstDescriptorForTarget` (the `is`/`as` target
// descriptor) both fell back to the shared, erased class descriptor. Value and
// target then compared equal for every argument → a false-positive match
// (`Wrap<Array<int>> is Wrap<Array<string>>` → TRUE; `as` → non-null).
//
// SOUNDNESS. A sema-only `typeArgRuntimeReified` update that admitted these
// tests (#2013's stated follow-up) would UNSOUNDLY compile them and produce
// the false-positive at runtime — the descriptor-erasure root cause is in
// codegen, not sema. The complete fix is BOTH:
//   (a) codegen: descTypeArgConcrete admits a null-decl Named whose
//       genericArgs are non-empty and recursively concrete (mirroring the
//       appendDescKey recursion #2013 added, and the refMentionsTypeVariable
//       arity split that already distinguishes a bare parameter). The instance
//       descriptor BFS (instanceDescriptorFor) then mints a per-monomorphization
//       descriptor for `Wrap<Array<int>>` distinct from `Wrap<Array<string>>`.
//       descReadableMangle recurses for a readable cosmetic global name.
//   (b) sema: typeArgRuntimeReified mirrors (a) so the plain `is`/`as` erasure
//       guard (#2006) no longer rejects these (now-sound) tests.
//
// WHAT THIS TEST PINS. The runtime `is`/`as` behavior — both directions, plus
// non-regression on the existing reified-arg and erased-arg cases. Most tests
// observe a bool via the process exit code (1 = true, 0 = false). WITHOUT the
// fix the test programs are REJECTED at compile time by the #2006 erasure guard
// (the conservative sound posture main shipped); WITH the fix they compile and
// produce the spec-correct runtime result. (No ASan OOB is exhibited for
// `Wrap<W>` because its W is phantom / reference-shaped, so the two layouts are
// byte-identical; the bug is the type-confusion soundness hole itself — a
// target whose differing arg changes layout would also be memory-unsafe.)

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Wrap<W>: a phantom generic whose W is only a type argument, never stored —
// isolates the descriptor-key recursion under test (so a layout-affecting
// difference is not what the type test rides on). Reused across cases.
int runProg(const std::string& mainBody) {
    return run(
        "module M; "
        "class Wrap<W> public tag: int; "
        "  public constructor(t: int) tag := t; end constructor; end Wrap; "
        "class C public static method Main(): int " + mainBody +
        " end Main; end C;");
}

}  // namespace

// ---- DIRECT null-decl-builtin arg: the exact reported false positive ----------

// The reported defect: `Wrap<Array<int>> is Wrap<Array<string>>` was a false-
// positive TRUE pre-fix (descriptor collapsed to shared Wrap). Must be FALSE —
// the runtime mints DISTINCT descriptors, so the arg differs (FAILS WITHOUT
// the fix: #2006's erasure guard rejected the test at compile time → r.ran is
// false → run returns -999; never reaches this assertion's expected value).
BEZEL_TEST(codegen_2022, direct_is_wrong_arg_is_false) {
    BEZEL_EXPECT_EQ(runProg(
        "using var ai := new Array<int>(); ai.Add(5); "
        "using var w := new Wrap<Array<int>>(7); "
        "var o: Object := w; "
        "return (if o is Wrap<Array<string>> then 1 else 0);"), 0);
}

// The genuinely-matching arg still matches (TRUE). Pins both directions of the
// `is` correctness — must not over-correct into a false negative.
BEZEL_TEST(codegen_2022, direct_is_right_arg_is_true) {
    BEZEL_EXPECT_EQ(runProg(
        "using var ai := new Array<int>(); ai.Add(5); "
        "using var w := new Wrap<Array<int>>(7); "
        "var o: Object := w; "
        "return (if o is Wrap<Array<int>> then 1 else 0);"), 1);
}

// `as` to the wrong arg yields null (§17.4) — no reinterpretation. This is the
// memory-unsafety half: a non-null result would let the program bind a
// Wrap<Array<int>> at the wrong static type Wrap<Array<string>>.
BEZEL_TEST(codegen_2022, direct_as_wrong_arg_is_null) {
    BEZEL_EXPECT_EQ(runProg(
        "using var ai := new Array<int>(); ai.Add(5); "
        "using var w := new Wrap<Array<int>>(7); "
        "var o: Object := w; "
        "var x := o as Wrap<Array<string>>; "
        "return (if x = null then 1 else 0);"), 1);
}

// `as` to the matching arg succeeds (non-null).
BEZEL_TEST(codegen_2022, direct_as_right_arg_non_null) {
    BEZEL_EXPECT_EQ(runProg(
        "using var ai := new Array<int>(); ai.Add(5); "
        "using var w := new Wrap<Array<int>>(7); "
        "var o: Object := w; "
        "var x := o as Wrap<Array<int>>; "
        "return (if x <> null then 1 else 0);"), 1);
}

// ---- THE HEADLINE: combined is/as, both directions in one program -------------

// The exact repro from #2022: r1 (true positive) * 100 + r2 (false-positive
// avoidance) * 10 + r3 (as=null). Pre-fix base = compile error; pre-fix unsound
// sema-only = 110 (r1=1, r2=1, r3=0 — false positive); post-fix correct = 101
// (r1=1, r2=0, r3=1). Pins the WHOLE soundness contract in one exit code.
BEZEL_TEST(codegen_2022, combined_repro_is_as_correct) {
    BEZEL_EXPECT_EQ(runProg(
        "using var ai := new Array<int>(); ai.Add(5); "
        "using var w := new Wrap<Array<int>>(7); "
        "var o: Object := w; "
        "var r1 := (if o is Wrap<Array<int>> then 1 else 0); "
        "var r2 := (if o is Wrap<Array<string>> then 1 else 0); "
        "var x := o as Wrap<Array<string>>; "
        "var r3 := (if x = null then 1 else 0); "
        "return r1 * 100 + r2 * 10 + r3;"), 101);
}

// ---- MULTI-PARAM null-decl builtin: Dictionary<K,V> value-arg distinguishes ----

// `Wrap<Dictionary<int,string>>` vs `Wrap<Dictionary<int,long>>` — the second
// argument differs. Pre-fix both collapsed to the shared Wrap descriptor →
// false positive. Post-fix the keys differ → FALSE. Pins multi-arg recursion.
BEZEL_TEST(codegen_2022, multi_param_dictionary_value_arg_distinguishes) {
    BEZEL_EXPECT_EQ(runProg(
        "using var d := new Dictionary<int, string>(); d.Add(1, \"a\"); "
        "using var w := new Wrap<Dictionary<int, string>>(7); "
        "var o: Object := w; "
        "var r1 := (if o is Wrap<Dictionary<int, string>> then 1 else 0); "
        "var r2 := (if o is Wrap<Dictionary<int, long>> then 1 else 0); "
        "var x := o as Wrap<Dictionary<int, long>>; "
        "var r3 := (if x = null then 1 else 0); "
        "return r1 * 100 + r2 * 10 + r3;"), 101);
}

// ---- NESTED null-decl builtin: the arg distinction must hold at every depth ----

// `Wrap<Array<Array<int>>>` vs `Wrap<Array<Array<string>>>` — the differing
// argument is buried TWO levels deep inside null-decl builtins. Pins that the
// recursion reaches it (r1 = matching, r2 = mismatching → 10).
BEZEL_TEST(codegen_2022, nested_array_of_array_distinguishes) {
    BEZEL_EXPECT_EQ(runProg(
        "using var w := new Wrap<Array<Array<int>>>(7); "
        "var o: Object := w; "
        "var r1 := (if o is Wrap<Array<Array<int>>> then 1 else 0); "
        "var r2 := (if o is Wrap<Array<Array<string>>> then 1 else 0); "
        "return r1 * 10 + r2;"), 10);
}

// ---- CONTROL: existing reified-arg behavior must NOT regress ------------------

// `Wrap<int> is Wrap<string>` — primitive arg differs → still FALSE. The fix
// must not affect the existing primitive-arg descriptor behavior.
BEZEL_TEST(codegen_2022, control_primitive_arg_wrong_is_false) {
    BEZEL_EXPECT_EQ(runProg(
        "using var w := new Wrap<int>(7); "
        "var o: Object := w; "
        "return (if o is Wrap<string> then 1 else 0);"), 0);
}

// `Wrap<int> is Wrap<int>` — same primitive arg → still TRUE.
BEZEL_TEST(codegen_2022, control_primitive_arg_right_is_true) {
    BEZEL_EXPECT_EQ(runProg(
        "using var w := new Wrap<int>(7); "
        "var o: Object := w; "
        "return (if o is Wrap<int> then 1 else 0);"), 1);
}

// ---- CONTROL: dedup preserved — two Wrap<Array<int>> values share one ---------

// A second, independently-constructed Wrap<Array<int>> must still be
// `is Wrap<Array<int>>`. The fix SPLITS on differing args; it must not stop
// deduping IDENTICAL args. Both m1 and m2 must be 1 → 11.
BEZEL_TEST(codegen_2022, control_same_arg_dedup_both_match) {
    BEZEL_EXPECT_EQ(runProg(
        "using var ai := new Array<int>(); ai.Add(5); "
        "using var w1 := new Wrap<Array<int>>(1); "
        "using var w2 := new Wrap<Array<int>>(2); "
        "var o1: Object := w1; var o2: Object := w2; "
        "var m1 := (if o1 is Wrap<Array<int>> then 1 else 0); "
        "var m2 := (if o2 is Wrap<Array<int>> then 1 else 0); "
        "return m1 * 10 + m2;"), 11);
}

// ---- CONTROL: both null-decl-builtin instantiations used in one module --------

// A module that uses BOTH `Wrap<Array<int>>` and `Wrap<Array<string>>` with
// real method calls must compile, link (two same-named internal descriptors are
// LLVM-uniquified — no duplicate-symbol), and run to the spec-stated value with
// no codegen crash. Cross-tests both directions (the values are kept compact so
// the sum fits in the 8-bit process exit code that `runAot` returns). With
// wi.Label=1, ws.Label=2, and t1/t3 = 1 (true positives on the matching arg),
// t2/t4 = 0 (false-positive avoidance on the mismatching arg):
//   s(3) + 10*t1(10) + 20*t2(0) + 30*t3(30) + 40*t4(0) = 43.
BEZEL_TEST(codegen_2022, both_instantiations_compile_link_and_run_precise) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Wrap<W> public tag: int; public Label: int; "
        "  public constructor(t: int, lbl: int) tag := t; Label := lbl; "
        "  end constructor; "
        "  public method GetLabel(): int return Label; end GetLabel; "
        "end Wrap; "
        "class C public static method Main(): int "
        "  using var ai := new Array<int>(); ai.Add(10); ai.Add(20); "
        "  using var asx := new Array<string>(); asx.Add(\"x\"); "
        "  using var wi := new Wrap<Array<int>>(7, 1); "
        "  using var ws := new Wrap<Array<string>>(9, 2); "
        "  var s := wi.GetLabel() + ws.GetLabel(); "          // 1 + 2 = 3
        "  var oi: Object := wi; var os: Object := ws; "
        "  var t1 := (if oi is Wrap<Array<int>> then 1 else 0); "    // 1
        "  var t2 := (if oi is Wrap<Array<string>> then 1 else 0); " // 0
        "  var t3 := (if os is Wrap<Array<string>> then 1 else 0); " // 1
        "  var t4 := (if os is Wrap<Array<int>> then 1 else 0); "    // 0
        "  return s + 10*t1 + 20*t2 + 30*t3 + 40*t4; "        // 3+10+0+30+0=43
        "end Main; end C;"), 43);
}

// ---- CONTROL: a FUNCTION-typed arg must still be REJECTED (#2006 non-regress) --

// A genuinely-erased kind (function type — appendDescKey emits "F" at any depth,
// so the descriptor still collapses) MUST stay rejected by the #2006 erasure
// guard. The fix admits ONLY null-decl-builtins with recursively-reified args;
// it must not bleed into the Function kind. Pins that the runtime descriptor
// minting admission and the sema admission are scoped to the SAME arg set.
BEZEL_TEST(codegen_2022, control_function_arg_still_rejected) {
    auto r = runAot(
        "module M; "
        "class Wrap<W> public tag: int; "
        "  public constructor(t: int) tag := t; end constructor; end Wrap; "
        "class C public static method Main(): int "
        "  using var w := new Wrap<(int) -> int>(7); "
        "  var o: Object := w; "
        "  return (if o is Wrap<(string) -> string> then 1 else 0); "
        "end Main; end C;");
    // The #2006 erasure guard fires at sema → runAot's ran is false and the
    // diagnostics carry the guard's "is`/`as` target is not runtime-
    // distinguishable" error message.
    BEZEL_EXPECT_FALSE(r.ran);
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("`is`/`as` target is not runtime-distinguishable") !=
                std::string::npos)
            found = true;
    }
    BEZEL_EXPECT_TRUE(found);
}
