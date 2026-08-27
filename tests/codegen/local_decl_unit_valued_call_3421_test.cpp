// fix #3421 — binding a UNIT-valued call to a local CRASHED the compiler
// (spec §4.7 *The unit type*, §21.1 *Variable declarations*, §8.5 *`ToString`*,
// §8.6 *Boxing*, §30.3 *The `Task`, `Task<T>`, and `OwningTask<T>` types* —
// v0.283).
//
// MEASURED on 0e95d36 (the #3391 fix, this file's parent commit):
//
//     var r := C.Nothing();     ->  bezelc: SIGILL, core dumped, exit 132,
//                                   with NO diagnostic of any kind
//
// and five more spellings of the same thing, at three different LLVM entry
// points: `lowerLocalDecl`'s `CreateStore` and the assignment statement's
// `CreateStore` both trap in `DataLayout::getABITypeAlign` (an
// `llvm_unreachable` that this libLLVM build compiles to `ud2`), and
// `boxValue`'s `getTypeAllocSize` fails `Ty->isSized()` (abort, exit 134).
//
//   var r := C.Nothing();            132 (SIGILL)   T1
//   var r: () := C.Nothing();        132 (SIGILL)   T2
//   var o: Object := C.Nothing();    134 (assert)   T3
//   var n: ()? := C.Nothing();       132 (SIGILL)   T4
//   r := C.Nothing();                132 (SIGILL)   T5
//   n := C.Nothing();                132 (SIGILL)   T6
//
// ── IS IT LEGAL BEZEL? THE DOCUMENTS SAY YES ─────────────────────────────────
//
// This had to be settled before a line was written, because "reject it in sema"
// and "lower it" are different fixes. §4.7, in full:
//
//   "`()` is the type with exactly one value, also written `()`. It is the
//    return type of methods declared without an explicit return type."
//
//   "The unit type is a value type and can in principle be boxed via the
//    `Object` derivation chain ([§8.6]) — `var o: Object := ();` is
//    grammatically valid and produces a boxed `()`."
//
// The first sentence gives `C.Nothing()` — a method declared with no return
// type — the type `()`. The second SANCTIONS THE EXACT STATEMENT SHAPE at issue:
// a `var` declaration whose initializer is a unit-typed expression, at an
// `Object` target no less. §21.1 places no restriction on a `var` initializer's
// type (its only prohibitions are the `_` discard spelling and the runtime-
// `const` ownership rule). And three further rules independently REQUIRE a unit
// value to be storable in a location:
//
//   * §3.2: "A **value type** is copied on assignment; each variable holds an
//     independent value. Records, enums, choices ([§35]), primitives, inline
//     arrays (`[N]T`, including `[N]T?` and `[N owned]T?`/`[N owned]T`), and
//     the unit type are value types."
//   * §17.11 / §3.6's *defaultable* row: the unit type is defaultable, and
//     §17.11's per-kind list gives its zero value — "The unit type: `()`" — so
//     `default(())` is an expression whose value must go somewhere.
//   * §14.2: "`where T: value` restricts T to non-nullable value types —
//     records, enums, choices ([§35]), primitives, inline arrays (all forms,
//     including `[N owned]…` owning forms), and the unit type", so a generic
//     body's `var x: T := f();` must compile at T = `()`.
//
// So it is LEGAL, this is a codegen gap and not a missing rejection, and the
// answer is to lower it — which is also what #3391 (the parent commit) left
// deliberately open when it made a member ACCESS on a `()` receiver a hard
// error: `()` has no `Id`, but `()` is still a value.
//
// ── WHY THE FIX IS AT THE PRODUCER ───────────────────────────────────────────
//
// The backend is written throughout against ONE invariant: a unit-typed
// expression lowers to `nullptr`. The `()` LITERAL has always obeyed it, and
// every unit rescue keys on `!v` — `boxForTarget`'s #2915 arm, its #3095 `()?`
// sibling, `unitValueForTarget`, `rescueUnitAssignRhs`, `lowerLocalDecl`'s
// #2434 zero-init fallback. A DIRECT call to a unit-returning method broke it:
// `CreateCall` hands back a real void-typed `CallInst` and `lowerExpr` returned
// it verbatim, so every consumer's `if (v)` took the has-a-value branch on a
// value that does not exist.
//
// That the producer is the right layer is settled by T9 below: the CLOSURE form
// `var f := C.Nothing; var r := f();` ALREADY compiled and ran correctly before
// this fix, because the closure-call path performs this very normalization at
// its own return (`retTy->isVoidTy() ? nullptr : r`). The direct-call form
// differs from it in nothing the language recognises. So `lowerExpr` now
// normalizes every void-typed result to `nullptr`, once, and the three crashing
// consumers reach their pre-existing and already-correct unit paths — which is
// why T3 does not merely stop crashing but prints §8.5's `"()"`, and T4/T6 land
// on §4.7's PRESENT `()?` rather than its `null`.
//
// This also stops the bleeding: #2915, #3095 and #3105 were three separately
// filed, separately fixed per-consumer repairs of this one root cause, and
// #3421 was the fourth. `unitValueForTarget` is correspondingly widened to
// answer the plain `()` target as well as `()?`, so it is now the ONE predicate
// saying what a unit source lowers to at a target, instead of the copy each
// argument site carried (#3419).
//
// ── WHAT MAKES THIS A REGRESSION TEST AND NOT A SMOKE TEST ───────────────────
//
// The pre-fix failure is a COMPILER CRASH, so "something happened" is not an
// assertion. Every case below therefore pins three things: `r.ran` (the compile
// did not die), a specific exit code, and the exact stdout. T7 pins the SIDE
// EFFECT COUNT — a fix that dropped the call along with its non-existent result
// would satisfy "no crash" and be silently wrong — and T8/T9/T10 are controls
// on the shapes that already worked, so the producer change cannot be shown
// green by having quietly broken them.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// A method declared with NO return type — §4.7's "the return type of methods
// declared without an explicit return type" — plus the `: ()` spelling of the
// same thing, which §32.5 confirms means the same, for the FFI declarations it
// governs: "An FFI declaration with no `:` clause produces a unit-returning
// method; an explicit `: ()` is also accepted and means the same thing".
constexpr const char* kDecls =
    "module M3421; "
    "class C "
    "  public static method Nothing() "
    "    Console.WriteLine(\"side\"); "
    "  end Nothing; "
    "  public static method NothingAscribed(): () "
    "    Console.WriteLine(\"side2\"); "
    "    return; "
    "  end NothingAscribed; ";

std::string prog(const std::string& body) {
    return std::string(kDecls) +
           "  public static method Main(): int " + body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// ── T1 — the issue's exact repro: an INFERRED-type local bound to a call on a
// method declared without a return type. Pre-fix: SIGILL, exit 132, no
// diagnostic. Post-fix: the call runs (its `Console.WriteLine` is observable),
// the local holds §4.7's one `()` value, and `Main` returns normally.
BEZEL_TEST(codegen_local_decl_unit_valued_call_3421,
           inferred_local_binds_unit_call_spec_4_7_21_1) {
    auto r = runAot(prog(
        "    Console.WriteLine(\"a\"); "
        "    var r := C.Nothing(); "
        "    Console.WriteLine(\"b\"); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("a\nside\nb\n"));
}

// ── T2 — the ASCRIBED spelling `var r: () := …`. The issue records that this
// "crashes identically", and it must: §4.7 makes the two forms the same
// declaration, one with the type written out and one inferred. The bound value
// renders as §8.5's `"()"`, which pins that the local really holds the unit
// value rather than merely surviving.
BEZEL_TEST(codegen_local_decl_unit_valued_call_3421,
           ascribed_unit_local_binds_unit_call_spec_4_7_8_5) {
    auto r = runAot(prog(
        "    var r: () := C.Nothing(); "
        "    Console.WriteLine(r.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("side\n()\n"));
}

// ── T3 — the §4.7 sentence that settles legality, with the initializer a CALL
// instead of the literal it is written with: "`var o: Object := ();` is
// grammatically valid and produces a boxed `()`". Pre-fix this did not SIGILL
// but aborted on `Ty->isSized()` inside `boxValue` (exit 134). Post-fix the
// value is boxed and renders as §8.5's `"()"` — the same answer #2915 pinned for
// the literal form, which is the point: §4.7 draws no distinction between the
// two initializers, so neither may the backend.
BEZEL_TEST(codegen_local_decl_unit_valued_call_3421,
           object_local_boxes_unit_call_spec_4_7_8_5_8_6) {
    auto r = runAot(prog(
        "    var o: Object := C.Nothing(); "
        "    Console.WriteLine(o.ToString()); "
        "    var lit: Object := (); "
        "    Console.WriteLine(lit.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("side\n()\n()\n"));
}

// ── T4 — the `()?` target. §4.7: "Wrapping the unit type with `?` (yielding
// `()?`) is permitted by the grammar and produces an optional with TWO VALUES,
// `null` and `()`." So the two must be distinguishable, and a fix that merely
// stopped the SIGILL by dropping the store would collapse them — the local
// would read `null` where §4.7 says `()`. Both halves are asserted, so the
// present value cannot be shown green by a slot that is null either way.
BEZEL_TEST(codegen_local_decl_unit_valued_call_3421,
           nullable_unit_local_binds_unit_call_present_spec_4_7) {
    auto r = runAot(prog(
        "    var n: ()? := C.Nothing(); "
        "    Console.WriteLine((n <> null).ToString()); "
        "    var z: ()? := null; "
        "    Console.WriteLine((z = null).ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("side\nTrue\nTrue\n"));
}

// ── T5 / T6 — the ASSIGNMENT forms. A declaration is not the only value-
// demanding location §21.1 admits; the same void value reached the assignment
// statement's own `CreateStore` and trapped there too (a DIFFERENT LLVM entry
// point from T1's, which is why fixing only `lowerLocalDecl` would have left
// this standing). T6 again pins the §4.7 PRESENT `()?`, not merely no-crash.
BEZEL_TEST(codegen_local_decl_unit_valued_call_3421,
           assign_unit_local_from_unit_call_spec_4_7_21_1) {
    auto r = runAot(prog(
        "    var r: () := (); "
        "    r := C.Nothing(); "
        "    Console.WriteLine(r.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("side\n()\n"));
}

BEZEL_TEST(codegen_local_decl_unit_valued_call_3421,
           assign_nullable_unit_local_from_unit_call_spec_4_7) {
    auto r = runAot(prog(
        "    var n: ()? := null; "
        "    n := C.Nothing(); "
        "    Console.WriteLine((n <> null).ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("side\nTrue\n"));
}

// ── T7 — THE SIDE-EFFECT COUNT. The whole failure is "the compiler was handed a
// result that does not exist"; the obvious wrong fix is to stop lowering the
// initializer at all, which would pass every no-crash assertion above while
// silently deleting a call the program made. §21.1's initializer is an ordinary
// evaluated expression, so each of these five bindings must call EXACTLY ONCE,
// in source order. The interleaved markers pin the order as well as the count.
BEZEL_TEST(codegen_local_decl_unit_valued_call_3421,
           unit_call_initializer_evaluated_exactly_once_spec_21_1) {
    auto r = runAot(prog(
        "    var a := C.Nothing(); "
        "    Console.WriteLine(\"1\"); "
        "    var b: () := C.Nothing(); "
        "    Console.WriteLine(\"2\"); "
        "    var c: Object := C.Nothing(); "
        "    Console.WriteLine(\"3\"); "
        "    var d: ()? := C.Nothing(); "
        "    Console.WriteLine(\"4\"); "
        "    var e := C.NothingAscribed(); "
        "    Console.WriteLine(\"5\"); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("side\n1\nside\n2\nside\n3\nside\n4\n"
                                "side2\n5\n"));
}

// ── T8 — CONTROL: the `()` LITERAL initializer forms, which worked before this
// fix and must still. §4.7 gives them the same type as the call forms above, so
// the two must agree; this is the half of that agreement the fix must not move.
BEZEL_TEST(codegen_local_decl_unit_valued_call_3421,
           unit_literal_initializer_forms_unchanged_spec_4_7) {
    auto r = runAot(prog(
        "    var u: () := (); "
        "    Console.WriteLine(u.ToString()); "
        "    var p: ()? := (); "
        "    Console.WriteLine((p <> null).ToString()); "
        "    var q: () := (); "
        "    q := (); "
        "    Console.WriteLine(q.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("()\nTrue\n()\n"));
}

// ── T9 — CONTROL, and the evidence that fixed the LAYER. `var f := C.Nothing;
// var r := f();` already compiled and ran correctly pre-fix, because the
// closure-call path normalizes its own void return to `nullptr`. It is the same
// §4.7 unit value reaching the same §21.1 declaration; only the callee spelling
// differs. Pinning it here keeps the two spellings tied together, so a later
// change cannot make the direct form disagree with the closure form again.
// (§27.1 *Function types*: "A method without a return type has function type
// returning `()`".)
BEZEL_TEST(codegen_local_decl_unit_valued_call_3421,
           closure_call_form_binds_unit_unchanged_spec_4_7) {
    auto r = runAot(prog(
        "    var f := C.Nothing; "
        "    var r := f(); "
        "    Console.WriteLine(r.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("side\n()\n"));
}

// ── T10 — CONTROL on the ARGUMENT positions the producer change also feeds.
// #3105 fixed those by detecting the void value at each call site; with the
// producer normalized they now take the pre-existing `!v` rescue instead, so
// this pins that a unit-typed argument still arrives — at a plain `()` formal,
// at a `()?` formal, and via the `?.` spelling — for both the literal and the
// call source. A regression here would show as a dropped call or an IR-verifier
// failure, not as a wrong value, so `r.ran` is load-bearing.
BEZEL_TEST(codegen_local_decl_unit_valued_call_3421,
           unit_call_argument_positions_unchanged_spec_4_7_3105) {
    auto r = runAot(
        "module M3421B; "
        "class C "
        "  public static method Nothing() "
        "    Console.WriteLine(\"side\"); "
        "  end Nothing; "
        "  public static method Take(u: ()) "
        "    Console.WriteLine(\"took\"); "
        "  end Take; "
        "  public static method TakeN(u: ()?) "
        "    Console.WriteLine(\"tookN\"); "
        "  end TakeN; "
        "  public method TakeI(u: ()) "
        "    Console.WriteLine(\"tookI\"); "
        "  end TakeI; "
        "  public static method Main(): int "
        "    C.Take(()); "
        "    C.Take(C.Nothing()); "
        "    C.TakeN(()); "
        "    C.TakeN(C.Nothing()); "
        "    var o: C? := new C(); "
        "    o?.TakeI(C.Nothing()); "
        "    o?.TakeI(()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("took\nside\ntook\ntookN\nside\ntookN\n"
                                "side\ntookI\ntookI\n"));
}

// ── T11 — the §30.3 sibling, and the shape #3391 came from. §30.3 declares
// `public method Wait();` with no return type, so §4.7 makes `t.Wait()` a unit
// value and binding it is this issue's shape with a stdlib callee. The issue
// records that this one did NOT crash pre-fix (the Task-family `Wait()` lowering
// already returned `nullptr` for its unit result, taking `lowerLocalDecl`'s
// #2434 fallback), which makes it a control: the fix must leave the already-
// correct stdlib path alone while making the user-method path agree with it.
// `t.Result` is asserted alongside, because §30.3 names `Result` — not the
// return of `Wait()` — as where a task's value is read (the #3391 ruling).
BEZEL_TEST(codegen_local_decl_unit_valued_call_3421,
           task_wait_unit_result_bound_to_local_spec_30_3_4_7) {
    auto r = runAot(
        "module M3421C; "
        "class C "
        "  private static async method Work(i: int): Task<int> "
        "    await Task.Delay(1); "
        "    return i * 2; "
        "  end Work; "
        "  public static method Main(): int "
        "    using var t := C.Work(7); "
        "    var w := t.Wait(); "
        "    Console.WriteLine(w.ToString()); "
        "    Console.WriteLine(t.Result.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("()\n14\n"));
}
