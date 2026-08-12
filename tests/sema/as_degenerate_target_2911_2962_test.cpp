// fix #2911 + fix #2962 — spec §17.4 "Degenerate value-type targets" (v0.241).
//
// ONE spec paragraph, two arms, one recorded principle: "`as` requires its
// result type `T?` to exist and to be informative."
//
//   * §17.4 / §15.1 (#2911): "An **inline-array target** (any form — `[N]T`,
//     `[N]T?`, and the owning forms, which cannot even be boxed, §8.6) is a
//     **compile error naming this rule**: §15.1 admits no nullable inline-array
//     form, so the operator's contract is unsatisfiable. The remedy is the `is`
//     test, whose §12.7 narrowing admits copying the value out: `if o is [3]int
//     then var a: [3]int := o; end;`."
//   * §17.4 / §4.7 (#2962): "The **unit type** is likewise a compile error as an
//     `as` target: `()` has exactly one value, so the operation could never
//     carry more information than `x is ()` (§12.7), which the diagnostic names
//     as the remedy — `()?` remains a well-formed type (§4.7); the rejection is
//     of the operator form, not of the type."
//   * §17.4 / §3.6 rule 2: "When the target is a generic type parameter, the
//     check applies **per instantiation**: `x as T` at an inline-array or unit
//     substitution is rejected at the instantiation site."
//
// This commit lands the shared predicate and the #2911 (inline-array) pins; the
// #2962 (unit) pins are the next commit, in this same file, because they are
// the same paragraph and the same predicate.
//
// WHAT THE COMPILER DID BEFORE THIS FIX — every line measured on 309356e, not
// inferred, because a rejection that lands on code which compiles today has to
// be justified by what that code actually produced:
//
//   `var o: Object := a; var back := o as [3]int;`   -> 456   (correct since
//                                                              #2804)
//   `var miss := o as [4]int; miss[0]`               -> 0     with NO diagnostic
//        — a FAILED cast yielding an all-zero array indistinguishable from a
//          legitimately zero-valued one. The `null` §17.4 promises had nowhere
//          to live: §15.1 admits no `([4]int)?` for it to live in. THAT silent
//          wrong value is what this fix eliminates (#2911).
//   `o as [3]int?`                                   -> compiled clean
//   `o as [3 owned]D`                                -> compiled, emitting only
//        codegen's #2775 "this type test was DROPPED and reads as false"
//        internal warning.
//   `o as ()` and `o as ()?`                         -> compiled clean. #2962
//        recorded that "whether `x as ()` is even accepted by sema today was
//        not measured"; it was, here, and sema accepted BOTH silently. Codegen
//        fell through to the reference-target arm and produced a bare pointer
//        typed `()?` — a value of undefined shape (#2915 had already excluded
//        `Kind::Unit` from the unbox arm: the unit type lowers to LLVM `void`
//        and `load void` is not constructible).
//   `class Box<T>` with `var r := o as T;`, at `new Box<[3]int>()`
//                                                    -> SEGFAULTED the compiler
//        (exit 139, core dumped) on the executable path; at `Box<()>` it emitted
//        two #2434 "initializer expression failed to lower … degraded to a
//        zero-initialized value" internal warnings. The per-instantiation arm is
//        not a formality — it replaces a compiler crash with a diagnostic.
//
// The ACCEPT-CONTROLS below are half this file on purpose. A rejection that is
// too wide is a worse defect than the one it fixes, so every neighbouring form
// that must keep compiling is pinned: a reference target, a value target
// (`o as int` -> `int?`), an already-nullable target, `()?` AS A TYPE (which
// §17.4 explicitly protects), the `is` test at both degenerate targets, and
// `as T` at an instantiation that binds T to an ordinary type.
//
// The remedy tests are load-bearing in their own right — a diagnostic that
// names a remedy which does not work is worse than one that names none — and
// they carry the one uncomfortable finding of this fix, stated on each of them:
// the `is` TEST works and discriminates, but the COPY-OUT half of §17.4's
// printed remedy silently yields an all-zero array, because §12.7 narrowing to
// a value type is never unboxed by codegen at all (an `int` there reads the box
// POINTER: 866251552). That is PRE-EXISTING and orthogonal — none of the
// programs that show it contains an `as` — and is filed as **#3090**, with the
// instruction to strengthen these tests when it lands.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// True iff SOME error diagnostic carries the §17.4 inline-array arm.
bool rejectedInlineArrayTarget(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("admits no nullable inline-array form") !=
                std::string::npos &&
            d.message.find("§17.4") != std::string::npos)
            return true;
    return false;
}

// True iff the rejection was reported AT AN INSTANTIATION SITE (§3.6 rule 2)
// rather than inside the generic body.
bool rejectedAtInstantiationSite(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("rejected at the instantiation site") !=
                std::string::npos &&
            d.message.find("§3.6 rule 2") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// =====================================================================
// #2911 — §17.4 / §15.1: the inline-array arm.
// =====================================================================

// The issue's cardinal repro. Pre-fix this compiled and printed `456` then `0`,
// the `0` being a FAILED cast's all-zero array.
BEZEL_TEST(sema_as_degenerate_target_2911,
           inline_array_target_rejected_spec_17_4_15_1) {
    JitResult r = runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    var a: [3]int := [4, 5, 6]; "
        "    var o: Object := a; "
        "    var back := o as [3]int; "
        "    var miss := o as [4]int; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedInlineArrayTarget(r));
}

// §15.1: "The element type may be nullable (`[3]int?` is well-formed and denotes
// an inline array of nullable int)" — a DIFFERENT type, not a nullable array,
// and §17.4 names it in the rejected set ("any form — `[N]T`, `[N]T?`").
BEZEL_TEST(sema_as_degenerate_target_2911,
           nullable_element_inline_array_target_rejected_spec_17_4_15_1) {
    JitResult r = runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    var a: [3]int?; "
        "    var o: Object := a; "
        "    var back := o as [3]int?; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedInlineArrayTarget(r));
}

// §17.4's "and the owning forms, which cannot even be boxed, §8.6". Pre-fix this
// compiled with only codegen's #2775 dropped-test warning.
BEZEL_TEST(sema_as_degenerate_target_2911,
           owning_inline_array_target_rejected_spec_17_4_8_6) {
    JitResult r = runAot(
        "module M; "
        "class D public constructor() end constructor; end D; "
        "class C "
        "  public static method Main(): int "
        "    var o: Object := 7; "
        "    var back := o as [3 owned]D; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    bool named86 = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("admits no nullable inline-array form") !=
                std::string::npos &&
            d.message.find("cannot even be boxed") != std::string::npos &&
            d.message.find("§8.6") != std::string::npos)
            named86 = true;
    BEZEL_EXPECT_TRUE(named86);
}

// =====================================================================
// The remedies the diagnostics NAME must actually work.
// =====================================================================

// The inline-array arm's remedy, tested — which is how #2911's closing
// instruction ("the same test should verify that #2804's `o is [3]int`
// narrowing + copy-out actually works, since the diagnostic points at it")
// gets discharged. The answer it produced is worth stating plainly:
//
//   * the `is` TEST works, and DISCRIMINATES — `o is [3]int` true, `o is
//     [4]int` false. That is the property the rejected `as` could not deliver
//     at all (its miss yielded an all-zero array indistinguishable from a
//     legitimately zero one), and it is what this test asserts. Want 1.
//   * the COPY-OUT's value does NOT work yet. MEASURED on the fix build:
//     `var b: [3]int := o;` inside the narrowed block silently produces an
//     ALL-ZERO array, and the read after it draws #2961's "store dropped"
//     internal warning — because §12.7 narrowing to a value type is never
//     unboxed by codegen at all (`int` reads the box POINTER: 866251552; a
//     record throws OverflowException). Filed as **#3090**, which is a
//     PRE-EXISTING gap this fix neither causes nor touches: none of those
//     programs contains an `as`.
//
// ⚠ WHEN #3090 LANDS, STRENGTHEN THIS TEST: add `var b: [3]int := o;` inside
// the true branch and assert the recovered value 456, which is what §17.4's
// printed remedy line promises and what `o as [3]int` used to return before
// this rejection. The copy-out is deliberately NOT written here today — a test
// asserting the value would fail, and a test asserting the zero would pin the
// bug as correct. (tests/codegen/box_inline_array_2804_test.cpp's
// kUnboxRoundTripNowRejected carries the same instruction from the other side:
// it held the 456 assertion in its `as` spelling, and this fix re-points it at
// the rejection with the identical "restore it when #3090 lands" note.)
BEZEL_TEST(sema_as_degenerate_target_2911,
           remedy_is_test_discriminates_where_as_could_not_spec_12_7) {
    JitResult r = runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    var a: [3]int := [4, 5, 6]; "
        "    var o: Object := a; "
        "    var hit := 0; "
        "    if o is [3]int then hit := 1; end; "
        "    if o is [4]int then hit := hit + 8; end; "
        "    return hit; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// The copy-out SPELLING the diagnostic prints must at least be well-formed
// Bezel — a diagnostic that prints a line which does not even compile would be
// wrong on its face, independently of #3090's lowering gap. This asserts
// exactly that and no more: the program compiles and runs. Its VALUE is #3090's
// to assert (see the note above).
BEZEL_TEST(sema_as_degenerate_target_2911,
           remedy_copy_out_spelling_compiles_spec_12_7_17_4) {
    JitResult r = runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    var a: [3]int := [4, 5, 6]; "
        "    var o: Object := a; "
        "    if o is [3]int then "
        "      var b: [3]int := o; "
        "      if b[0] = 4 then return 0; end; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
}

// =====================================================================
// §3.6 rule 2 — the per-instantiation arm.
// =====================================================================

// TYPE-level parameter. Pre-fix: SEGFAULT (exit 139) compiling this program.
BEZEL_TEST(sema_as_degenerate_target_2911,
           generic_type_instantiated_at_inline_array_rejected_spec_3_6_rule_2) {
    JitResult r = runAot(
        "module M; "
        "class Box<T> "
        "  public constructor() end constructor; "
        "  public method Probe(o: Object): int "
        "    var r := o as T; "
        "    if r <> null then return 1; end; "
        "    return 0; "
        "  end Probe; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    var a: [3]int := [4, 5, 6]; "
        "    var o: Object := a; "
        "    using var b := new Box<[3]int>(); "
        "    return b.Probe(o); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedInlineArrayTarget(r));
    BEZEL_EXPECT_TRUE(rejectedAtInstantiationSite(r));
}

// METHOD-level parameter, bound by ARGUMENT INFERENCE. (The explicit spelling
// `Probe<[3]int>(o)` is not reachable: an inline array does not parse in a
// call's type-argument list on 309356e, measured — so inference is the only way
// in, and it is the path this covers.) Pre-fix: SEGFAULT (exit 139).
BEZEL_TEST(sema_as_degenerate_target_2911,
           generic_method_inferred_at_inline_array_rejected_spec_3_6_rule_2) {
    JitResult r = runAot(
        "module M; class C "
        "  public static method Probe<T>(o: Object, witness: T): int "
        "    var r := o as T; "
        "    if r <> null then return 1; end; "
        "    return 0; "
        "  end Probe; "
        "  public static method Main(): int "
        "    var a: [3]int := [4, 5, 6]; "
        "    var o: Object := a; "
        "    return C.Probe(o, a); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedInlineArrayTarget(r));
    BEZEL_EXPECT_TRUE(rejectedAtInstantiationSite(r));
}

// =====================================================================
// ACCEPT-CONTROLS. Every one of these compiled before the fix and must
// compile after it; a rejection that swallows them is a worse defect.
// =====================================================================

// §17.4's own examples: a reference target keeps its `T?` result, a value target
// produces `int?`, and an ALREADY-nullable target is admitted (the `as` result
// is nullable either way). §4.7's `()?` is exercised AS A TYPE — the thing
// §17.4 explicitly does not reject ("`()?` remains a well-formed type … the
// rejection is of the operator form, not of the type"). Want 7 (1|2|4).
BEZEL_TEST(sema_as_degenerate_target_2911,
           ordinary_as_targets_and_unit_optional_type_still_accepted_spec_17_4) {
    JitResult r = runAot(
        "module M; "
        "class D public constructor() end constructor; end D; "
        "class C "
        "  public static method Main(): int "
        "    var code := 0; "
        "    using var d := new D(); "
        "    var od: Object := d; "
        "    if (od as D) <> null then code := code + 1; end; "
        "    var oi: Object := 42; "
        "    if (oi as int) <> null then code := code + 2; end; "
        "    if (oi as int?) <> null then code := code + 4; end; "
        "    var u: ()? := null; "
        "    if u <> null then code := code + 8; end; "
        "    return code; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// The rejection is of `as`, NOT of the target type in every operator: §12.7's
// `is` at exactly these two targets is what the diagnostics point at, so it must
// stay admitted — including through a generic parameter, where recording the
// obligation for `is` would have rejected the remedy itself. Compiles; the
// generic `is T` VALUE is deliberately not asserted (it reads false at an
// inline-array instantiation on 309356e — a separate, pre-existing defect,
// filed as #3092; asserting it here would pin a bug this fix does not touch).
BEZEL_TEST(sema_as_degenerate_target_2911,
           is_test_at_degenerate_targets_still_accepted_spec_12_7) {
    JitResult r = runAot(
        "module M; "
        "class Box<T> "
        "  public constructor() end constructor; "
        "  public method Probe(o: Object): int "
        "    if o is T then return 1; end; "
        "    return 0; "
        "  end Probe; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    var a: [3]int := [4, 5, 6]; "
        "    var o: Object := a; "
        "    var code := 0; "
        "    if o is [3]int then code := code + 1; end; "
        "    if o is not [4]int then code := code + 2; end; "
        "    var ou: Object := (); "
        "    if ou is () then code := code + 4; end; "
        "    using var b := new Box<[3]int>(); "
        "    var ignored := b.Probe(o); "
        "    return code; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// The per-instantiation arm must fire ONLY per instantiation: a generic body
// that writes `as T` is legal, and stays legal at every ordinary substitution —
// both for a TYPE parameter and for an inferred METHOD parameter. This is the
// control that catches a fix which rejected the BODY instead of the offending
// instantiations.
//
// It asserts that the program COMPILES AND RUNS, and deliberately not what it
// returns: `var r := o as T;` at a bare type parameter draws #2434's
// "initializer expression failed to lower … degraded to a zero-initialized
// value" internal fallback at EVERY substitution — measured identically on
// 309356e at `Box<int>`, so it is pre-existing and untouched here. Asserting an
// exit code would pin that separate gap.
BEZEL_TEST(sema_as_degenerate_target_2911,
           generic_as_target_at_ordinary_substitution_accepted_spec_3_6_rule_2) {
    JitResult r = runAot(
        "module M; "
        "class D public constructor() end constructor; end D; "
        "class Box<T> "
        "  public constructor() end constructor; "
        "  public method Probe(o: Object): int "
        "    var r := o as T; "
        "    if r <> null then return 1; end; "
        "    return 0; "
        "  end Probe; "
        "end Box; "
        "class C "
        "  public static method Probe<T>(o: Object, witness: T): int "
        "    var r := o as T; "
        "    if r <> null then return 1; end; "
        "    return 0; "
        "  end Probe; "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    var od: Object := d; "
        "    using var b := new Box<D>(); "
        "    var code := b.Probe(od); "
        "    if C.Probe(od, d) = 1 then code := code + 2; end; "
        "    return code; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    bool rejected = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("§17.4") != std::string::npos) rejected = true;
    BEZEL_EXPECT_FALSE(rejected);
}
