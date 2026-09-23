// Regression tests for issue #4665 (codegen; language spec §17.11 *The
// `default` operator* — the UNIT row — with §4.7 *The unit type*, §14.2
// *Constraints* and §22.5 *Optional parameters and named arguments*, v0.307):
// `default(T)` at the unit type killed `bezelc` with SIGILL and no diagnostic.
//
// ── WHAT WAS MEASURED, AND WHAT THE ISSUE GOT WRONG ─────────────────────────
//
// The issue was filed from an incidental sighting and offered two candidate
// mechanisms: (a) "the unit type has no value representation, so the return
// slot is zero-width and the epilogue reads garbage", or (b) "a substitution
// that leaves the return type unresolved, falling through to an
// `unreachable`-emitting default arm" — noting that (b) would be a much wider
// defect. It also stated that compilation SUCCEEDS and the fault is in the
// generated program.
//
// Both halves are refuted by measurement on 572f5fc5. `bezelc -c` itself dies:
//
//     $ bezelc -c -o p.o p.bzl        # the issue's own repro
//     Illegal instruction (core dumped)   rc=132, stdout empty, stderr empty
//
//     #0  llvm::Constant::getNullValue(llvm::Type*)        (libLLVM.so.21.1)
//     #1  Emitter::lowerExpr            (LlvmBackend_Exprs.cpp:43)
//     #2  Emitter::lowerStmt            (LlvmBackend_Stmts.cpp:2523)
//     #4  Emitter::buildSpecializedMethodFn (LlvmBackend_Calls2.cpp:14511)
//     #5  Emitter::emitGenericSpecialization(LlvmBackend_Calls2.cpp:13976)
//
// So it is mechanism (a), and it is the NARROW one: the substitution worked —
// `emitGenericSpecialization` resolved `T` to `()` correctly and handed the
// CONCRETE unit type to the `DefaultExpr` arm, which did
// `Constant::getNullValue(lowerType(()))`. `lowerType` answers LLVM `void` for
// exactly one thing (the unit type's `Kind::Unit` arm) and `getNullValue(void)`
// executes an `llvm_unreachable` that this release libLLVM compiles to `ud2`.
// No `unreachable` was emitted INTO the program and no substitution was left
// unresolved; the trap is in the compiler, on the same well-worn §4.7 seam as
// #1619 / #2827 / #3095 / #3105 / #3421 / #3527 / #3528.
//
// ── THE GENERIC IS NOT PART OF THE BUG ──────────────────────────────────────
//
// Minimised: the generic is only one route to a `default(())`. Writing it
// directly traps identically, which §17.11 says in as many words — "this row is
// reached by writing `default(())`, and is not confined to the `default(T)` the
// compiler supplies internally … `var u: () := default(());` and `var u: () :=
// ();` bind the same value." Measured on 572f5fc5, all rc=132:
//
//     var u: () := default(());                   // T2 below
//     var u := default(());                       // inferred, same trap
//     Take(p: () := default(()))  called as Take()  // T3 below, a SECOND site
//
// The third is `fillOmittedParamDefaults`, which deliberately lowers the
// PARAMETER's own annotation rather than the `DefaultExpr`'s inner type (#1163)
// and so inherited the same `void`. It differs from the value position in what
// it owes: `lowerParamType` has already put the §4.7 placeholder byte in the
// callee's signature, so that site must produce a real `i8 0` while the value
// position must produce NOTHING (`nullptr`, the backend's one representation of
// "this expression produced no lowered value", #3421).
//
// Not affected, and asserted as such below: `()?` (`isNullableValueTy` claims it
// first and `lowerType` answers the tagged `{i8, i8}` optional, whose all-zero
// null IS §17.11's "every nullable type `T?`" row), `[N]()` and a record with a
// unit component (both already route their element/member through
// `unitStorageTy`, #3527/#3528).
//
// ── WHY THESE ROWS ──────────────────────────────────────────────────────────
//
// Pre-fix every unit row fails at `r.ran` — the compiler died, so no binary
// exists. "It compiles" is therefore NOT the oracle here: a fix that stopped the
// trap by substituting a wrong-width or wrong-shaped value would compile and
// then misbehave, so every row RUNS the program and reads an observable back.
// §8.5 renders the unit value as `"()"`, which is the text a wrong substitution
// disturbs. T4 is the both-sides control.
//
// The DIAGNOSTIC half — that a correct unit return inside a specialized generic
// body no longer draws the #2491 "failed to lower" internal note — cannot live
// here: `runAot` populates `JitResult::diagnostics` only on a compile REJECT, so
// a successful compile's WARNINGS never reach it and an "assert no note" written
// in this file would pass vacuously on any compiler (the finding recorded in
// tests/codegen/call_lowered_as_unit_vs_declined_2963_test.cpp). It is pinned in
// tests/lit/default_unit_no_degrade_note_4665.bzl.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// ── T1 — THE ISSUE'S OWN REPRO: a generic method returning the bare type
// parameter `T`, instantiated at `()`. Pre-fix `bezelc` died of SIGILL
// (rc=132) inside `emitGenericSpecialization`, so `r.ran` is false. The `int`
// instantiation of the SAME method is exercised beside it, so a "fix" that
// disabled bare-`T`-return specialization altogether fails here too.
BEZEL_TEST(codegen_default_unit_4665,
           generic_bare_type_param_return_default_at_unit_spec_17_11_14_2) {
    auto r = runAot(
        "module M4665A; "
        "class Factory<T> "
        "  public method Make(): T return default(T); end Make; "
        "end Factory; "
        "class C "
        "  public static method Main(): int "
        "    using var fu := new Factory<()>(); "
        "    var u := fu.Make(); "
        "    Console.WriteLine(u.ToString()); "
        "    using var fi := new Factory<int>(); "
        "    var n := fi.Make(); "
        "    Console.WriteLine(n.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("()\n0\n"));
}

// ── T2 — THE MINIMAL REPRO, and §17.11's stated equivalence. No generic
// anywhere: `default(())` written directly is the §17.11 unit row. The spec
// says `var u: () := default(());` and `var u: () := ();` bind the same value,
// so the two spellings are rendered side by side and must agree — a
// substitution that produced some other value would print something else rather
// than fail to compile. The `Object` row additionally pins that the value still
// reaches §8.6 boxing through `boxForTarget`'s `!v` arm (#2915), which is what
// answering the placeholder byte here instead of `nullptr` would have broken.
BEZEL_TEST(codegen_default_unit_4665,
           default_of_unit_written_directly_spec_17_11_4_7) {
    auto r = runAot(
        "module M4665B; "
        "class C "
        "  public static method Main(): int "
        "    var d: () := default(()); "
        "    var l: () := (); "
        "    var i := default(()); "
        "    Console.WriteLine(d.ToString()); "
        "    Console.WriteLine(l.ToString()); "
        "    Console.WriteLine(i.ToString()); "
        "    var o: Object := default(()); "
        "    Console.WriteLine(o.ToString()); "
        "    Console.WriteLine($\"interp {d}\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("()\n()\n()\n()\ninterp ()\n"));
}

// ── T3 — THE SECOND SITE: §22.5's omitted-argument default for a unit-typed
// parameter (`fillOmittedParamDefaults`). This one owes a REAL operand — the
// callee's signature carries the §4.7 placeholder byte — so the omitted call
// and the explicit `()` call are asserted to agree; a site that answered
// nothing would produce a call with the wrong arity and a site that answered
// the wrong width would corrupt the neighbouring `string` argument, which is
// read back here. Pre-fix rc=132 in `fillOmittedParamDefaults` itself.
BEZEL_TEST(codegen_default_unit_4665,
           omitted_unit_parameter_default_spec_22_5_17_11) {
    auto r = runAot(
        "module M4665C; "
        "class C "
        "  public static method Take(tag: string, p: () := default(())): string "
        "    return tag + p.ToString(); "
        "  end Take; "
        "  public static method Main(): int "
        "    Console.WriteLine(C.Take(\"omitted=\")); "
        "    Console.WriteLine(C.Take(\"explicit=\", ())); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("omitted=()\nexplicit=()\n"));
}

// ── T4 — THE §22.2 ARROW BODY. `=> default(())` is the fourth route, and the
// one whose fix is a SECOND predicate (`arrowBodyLoweredAsUnit`) rather than
// the `DefaultExpr` arm. Pre-fix rc=132 like its siblings. This row is here and
// not only in the lit file because a predicate that suppresses a diagnostic can
// be "fixed" by breaking the lowering it describes: the arrow method is CALLED
// and the program's subsequent output is read back, so a `Give()` that stopped
// emitting its body — or a `ret void` that fell off the end of a non-void
// function — fails here while the lit oracle stays green.
BEZEL_TEST(codegen_default_unit_4665,
           arrow_body_default_of_unit_spec_22_2_17_11) {
    auto r = runAot(
        "module M4665E; "
        "class C "
        "  public N: int := 0; "
        "  public method Bump() N := N + 1; end Bump; "
        "  public static method Give(): () => default(()); "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    c.Bump(); "
        "    C.Give(); "
        "    c.Bump(); "
        "    C.Give(); "
        "    Console.WriteLine(c.N.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n"));
}

// ── T5 — THE NEIGHBOURING §17.11 ROWS, which must NOT change. `default(()?)`
// is the "every nullable type `T?`" row and is `null`; `default([3]())` and
// `default(R)` for a record with a unit component are the inline-array and
// record rows, both already routed through `unitStorageTy` (#3527/#3528). All
// three compiled and ran BEFORE this fix, so this row passes on both sides — it
// is the control that keeps a harness break or a syntax slip from reading as a
// green T1-T3, and it pins that the `DefaultExpr` guard fires for the
// NON-nullable unit type only.
BEZEL_TEST(codegen_default_unit_4665,
           neighbouring_default_rows_unchanged_spec_17_11) {
    auto r = runAot(
        "module M4665D; "
        "record R(A: int, U: (), B: int); "
        "class C "
        "  public static method Main(): int "
        "    var n: ()? := default(()?); "
        "    if n = null then Console.WriteLine(\"null\"); "
        "    else Console.WriteLine(\"present\"); end; "
        "    var a: [3]() := default([3]()); "
        "    Console.WriteLine(a[1].ToString()); "
        "    var r: R := default(R); "
        "    Console.WriteLine(r.ToString()); "
        "    var i: int := default(int); "
        "    Console.WriteLine(i.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("null\n()\nR(A: 0, U: (), B: 0)\n0\n"));
}
