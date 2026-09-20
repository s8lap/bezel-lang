// Regression tests for issue 4491 — a NAMED-argument §22.10 INHERITANCE
// CLAUSE (`class D(v: int) : B(s := "clause", n := v)`) was paired in
// TEXTUAL order (§22.5 "Optional parameters and named arguments" / §22.8
// step 0 / §22.10, v0.306).
//
// This is the case-(a) TWIN of #4484. #4484 fixed the `base(...)` CHAIN arm
// of `Emitter::prePopulateBasePositionalFields`; this file is the same defect
// on the arm that reads the §22.10 inheritance clause through
// `sema::primaryBaseCtorArgsOf`. Both arms live in one function and share its
// store and §22.5 default-fill loops, which is why the two fixes look alike —
// but they needed DIFFERENT publication channels and so are separate commits
// and separate files.
//
// WHAT §22.5 REQUIRES. Two sentences, both normative:
//
//   "Named arguments may appear in any order after positional arguments.
//    Each supplied argument binds exactly one parameter and each parameter
//    is bound by at most one supplied argument."
//
//   "A call's supplied argument expressions are evaluated in call-site
//    textual order — left to right as written, for positional and named
//    arguments alike; naming an argument changes which parameter it binds
//    to, never when it runs."
//
// and the section names the reach of the rule: "every construction site in
// the language reaches it through the same binding — a call, a `new`
// expression, and a record construction alike". A §22.10 inheritance clause
// is such a site: it supplies the BASE class's primary parameter list.
//
// WHAT THE COMPILER DID. Case (a) of `prePopulateBasePositionalFields`
// iterated the clause's `std::vector<ast::CallArgument>` positionally and
// never read `arg.name`, so clause argument `i` was stored into base
// positional field `i` whatever the argument was called.
//
// MEASURED ON e9239ab4 (the #4484 fix, i.e. the parent of this one), for the
// issue's own repro
//
//   class B(n: int, s: string) … end B;
//   class D(v: int) : B(s := "clause", n := v) … end D;
//   new D(9) … d.Go();
//
// the emitter stored the `string` pointer into the 4-byte `n` slot and the
// integer `9` into the `string` slot. The program printed `5811392` — the low
// 32 bits of the string pointer read as an `int` — and then died:
//
//   ERROR: AddressSanitizer: SEGV on unknown address 0x000000000011
//   READ memory access … #0 bz_string_to_utf8z
//     bezel_runtime_strings.cpp:1150 … #1 bz$M.B$Show #2 bz$M.D$Go
//
// (`0x11` is `9 << 1 | 1`, the integer `9` dereferenced as a small-string
// pointer.) `bezelc -c` exited 0 with no diagnostic, so this was a silent
// miscompile, not a missing feature.
//
// WHY A `decimal` PAYLOAD IS THE ASAN ORACLE, as in #4484's file: the
// `string` form above is an 8-byte store into a 4-byte slot, and ASan's
// 8-byte fast path reads the shadow granule the store STARTS in, which is
// valid memory — so the store is silent and only the later dereference
// faults. A 16-byte `decimal` bound BY NAME into what textual order makes the
// trailing 4-byte `int` slot crosses the end of the allocation and is
// reported at the store itself.
//
// HOW IT IS FIXED. Not with a second copy of the §22.5 rule in codegen. Sema
// publishes the clause's binding through the ONE binder body every other
// construction site runs (`bindArgsToParams` over
// `sema::FormalsView::of(recordPositionalFieldSymbols(base))`, #3173/#3178),
// and codegen's case (a) reads it and re-keys the store exactly as case (b)
// does. The CHANNEL had to be new — `SemaContext::inheritanceClauseBinding`,
// keyed on the declaring `ast::TypeDecl` — because the clause has no
// `ast::Expr` node to key on and the only Expr-shaped pointer available (a
// clause argument's own value) collides with the `new`-site binding whenever
// that argument is itself a `new` expression.
//
// WHICH ROWS ARE THE PINS. Seven rows fail on e9239ab4:
// `reversed_named_clause_args_*`, `reversed_named_decimal_clause_*`,
// `positional_then_named_clause_out_of_order_*`,
// `three_cycle_named_clause_permutation_*`,
// `named_clause_args_evaluate_in_textual_order_*`,
// `omitted_middle_clause_slot_*`, `named_unit_clause_argument_rescued_*` and
// `named_clause_owned_producer_checked_against_its_bound_slot_3750` — the
// last of which is the only row exercising the SEMA half of the fix, and the
// only FALSE-REJECTION pin in the file (every other pin is a wrong value or a
// crash). The remaining rows are CONTROLS that pass
// on both sides — declared-order names, an all-positional clause, and a
// clause binding an explicit base constructor (the `baseClauseExplicitCtor`
// guard this fix must leave undisturbed) — so a re-keying that broke a
// pairing which was already right fails here.
//
// ⚠ THE PERMUTATIONS ARE NOT INTERCHANGEABLE. `[1, 0]` and `[0, 2, 1]` are
// SELF-INVERSE, so a row using either cannot tell a `paramToArg` map from its
// inverse — the most plausible way to get this fix wrong. Only
// `three_cycle_named_clause_permutation_*` (`[1, 2, 0]`) and
// `omitted_middle_clause_slot_*` (`[1, -1, 0]`) have that discriminating
// power. Do not "simplify" the 3-cycle row into another two-argument swap.

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

// ---------------------------------------------------------------------------
// THE PIN (issue body's own repro, verbatim). `: B(s := "clause", n := v)`
// against `class B(n: int, s: string)`.
//
// Asserts the printed VALUES, not just the exit status: the loud→quiet trade
// (a guard that suppresses the store and leaves the object calloc-zero) is
// precisely what an exit-status oracle cannot see — the lesson #3562's own
// test file records about this very function.
// ---------------------------------------------------------------------------

BEZEL_TEST(inheritance_clause_named_arg_binding_4491,
           reversed_named_clause_args_bind_by_name_not_position_22_5) {
    auto r = runAot(
        "module M; "
        "class B(n: int, s: string) "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class D(v: int) : B(s := \"clause\", n := v) "
        "  public method Go() "
        "    Show(); "
        "  end Go; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(9); "
        "    d.Go(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9\nclause\n"));
}

// ---------------------------------------------------------------------------
// THE ASAN PIN. A 16-byte `decimal` bound BY NAME into what textual order
// makes the trailing 4-byte `int` slot. `runAotChecked` compiles the
// generated program with AddressSanitizer and flips `ran` to false on any
// sanitizer report, so pre-fix this row fails at `r.ran` with a
// "heap-buffer-overflow … WRITE of size 16" attached — the store itself, not
// a later read.
// ---------------------------------------------------------------------------

BEZEL_TEST(inheritance_clause_named_arg_binding_4491,
           reversed_named_decimal_clause_does_not_overflow_int_slot_22_5) {
    auto r = runAotChecked(
        "module M; "
        "class B(d: decimal, n: int) "
        "  public method Show() "
        "    Console.WriteLine(d.ToString()); "
        "    Console.WriteLine(n.ToString()); "
        "  end Show; "
        "end B; "
        "class D(v: int) : B(n := v, d := 7.5m) "
        "  public method Go() "
        "    Show(); "
        "  end Go; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var x := new D(1); "
        "    x.Go(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7.5\n1\n"));
}

// ---------------------------------------------------------------------------
// THE MIXED PIN. §22.5: "Named arguments may appear in any order AFTER
// positional arguments." `: B(v, k := 7, s := "m")` against
// `class B(n: int, s: string, k: int)` binds slot 0 positionally and then
// names the last two out of order, so textual order and §22.5 order differ on
// arguments 1 and 2. Pre-fix the integer `7` landed in the `string` slot and
// the string `"m"` in the trailing `int` slot.
// ---------------------------------------------------------------------------

BEZEL_TEST(inheritance_clause_named_arg_binding_4491,
           positional_then_named_clause_out_of_order_binds_by_name_22_5) {
    auto r = runAot(
        "module M; "
        "class B(n: int, s: string, k: int) "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "    Console.WriteLine(k.ToString()); "
        "  end Show; "
        "end B; "
        "class D(v: int) : B(v, k := 7, s := \"m\") "
        "  public method Go() "
        "    Show(); "
        "  end Go; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(1); "
        "    d.Go(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\nm\n7\n"));
}

// ---------------------------------------------------------------------------
// THE 3-CYCLE PIN — the row that distinguishes a BINDING from its INVERSE.
//
// Every other reversal row in this file uses a SELF-INVERSE permutation, and
// a permutation equal to its own inverse maps to ITSELF under a
// `paramToArg` / `argToParam` confusion — so none of them can detect an
// inverted map.
//
// `: B(c := 3, a := 1, b := 2)` against `class B(a: int, b: int, c: int)`
// binds `paramToArg = [1, 2, 0]`, a 3-cycle whose inverse `[2, 0, 1]` is a
// DIFFERENT map. Reading the table backwards yields `3 1 2`, which is also
// exactly what the pre-fix textual pairing produces, so this one row pins
// both the defect and the most plausible mis-fix of it.
//
// Every value is an `int`, so the pre-fix mispairing is non-fatal here and
// the row is a VALUE oracle rather than a crash oracle: measured on e9239ab4
// `3\n1\n2\n`, with this fix `1\n2\n3\n`.
// ---------------------------------------------------------------------------

BEZEL_TEST(inheritance_clause_named_arg_binding_4491,
           three_cycle_named_clause_permutation_binds_by_name_22_5) {
    auto r = runAot(
        "module M; "
        "class B(a: int, b: int, c: int) "
        "  public method Show() "
        "    Console.WriteLine(a.ToString()); "
        "    Console.WriteLine(b.ToString()); "
        "    Console.WriteLine(c.ToString()); "
        "  end Show; "
        "end B; "
        "class D() : B(c := 3, a := 1, b := 2) "
        "  public method Go() "
        "    Show(); "
        "  end Go; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.Go(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n2\n3\n"));
}

// ---------------------------------------------------------------------------
// THE EVALUATION-ORDER PIN. §22.5 *Evaluation order*: "A call's supplied
// argument expressions are evaluated in call-site TEXTUAL order — left to
// right as written, for positional and named arguments alike; naming an
// argument changes which parameter it binds to, never when it runs."
//
// That sentence is the whole justification for the SHAPE of the fix: the
// clause's argument-lowering loop stays in SOURCE order and `baseArgVals`
// stays source-indexed, and only the STORE is re-keyed by the §22.5 binding.
// A "simplification" that lowered in parameter order would keep every other
// row in this file green and silently invert the observable side-effect
// order, so the rule needs its own pin rather than only a comment.
//
// `: Base(s := Log.B(), n := Log.A())`: `Log.B()` is written first, so it
// runs first (`AtB = 1`) even though it binds the SECOND parameter, and
// `Log.A()` runs second (`AtA = 2`) even though it binds the first. The
// values still land by name (`n = 1`, `s = "x"`) — order and binding are
// independent. Pre-fix this program stores the string into the `int` slot and
// SIGSEGVs before printing anything.
// ---------------------------------------------------------------------------

BEZEL_TEST(inheritance_clause_named_arg_binding_4491,
           named_clause_args_evaluate_in_textual_order_but_bind_by_name_22_5) {
    auto r = runAot(
        "module M; "
        "class Log "
        "  public static Seq: int := 0; "
        "  public static AtA: int := 0; "
        "  public static AtB: int := 0; "
        "  public static method A(): int "
        "    Seq := Seq + 1; "
        "    AtA := Seq; "
        "    return 1; "
        "  end A; "
        "  public static method B(): string "
        "    Seq := Seq + 1; "
        "    AtB := Seq; "
        "    return \"x\"; "
        "  end B; "
        "end Log; "
        "class Base(n: int, s: string) "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end Base; "
        "class D() : Base(s := Log.B(), n := Log.A()) "
        "  public method Go() "
        "    Show(); "
        "  end Go; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.Go(); "
        "    Console.WriteLine(Log.AtA.ToString()); "
        "    Console.WriteLine(Log.AtB.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // n=1  s=x  AtA=2 (ran second)  AtB=1 (written first, ran first)
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\nx\n2\n1\n"));
}

// ---------------------------------------------------------------------------
// THE DEFAULT-FILL PIN. §22.5: "Every parameter that declares no default must
// be bound" and an unbound optional parameter takes its default — per SLOT.
// With named arguments the SUPPLIED slots are no longer a prefix:
// `: B(m := 9, n := 1)` against `class B(n: int, k: int := 5, m: int := 7)`
// supplies slots 0 and 2 and omits slot 1. (The parameter list still obeys
// §22.5's "every parameter after the first defaulted parameter must itself
// have a default" — the non-prefix shape comes from the NAMING, not from an
// illegal declaration.)
//
// This pins the half of the fix that is NOT the store: the §22.5 default-fill
// loop must ask the binding PER SLOT. Pre-fix the clause's two supplied
// values went into slots 0 and 1 and slot 2 took its declared default —
// measured `9\n1\n7\n`, every one of the three wrong.
// ---------------------------------------------------------------------------

BEZEL_TEST(inheritance_clause_named_arg_binding_4491,
           omitted_middle_clause_slot_takes_its_default_22_5) {
    auto r = runAot(
        "module M; "
        "class B(n: int, k: int := 5, m: int := 7) "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(k.ToString()); "
        "    Console.WriteLine(m.ToString()); "
        "  end Show; "
        "end B; "
        "class D() : B(m := 9, n := 1) "
        "  public method Go() "
        "    Show(); "
        "  end Go; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.Go(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n5\n9\n"));
}

// ---------------------------------------------------------------------------
// THE §4.7 UNIT-RESCUE PIN. `fieldForArg` is this fix's only other consumer
// on the clause arm, and #3502's unit rescue is its only non-defensive
// caller.
//
// §4.7: a unit-typed argument has NO lowered value — `()` lowers to LLVM
// `void`, so `lowerExpr` returns null — and #3502's rescue materializes the
// §4.7 placeholder for it, reading the TYPE of the positional slot the
// argument lands in. Pre-fix that slot was the argument's SOURCE position.
// For `: B(n := 1, u := C.Nothing())` against `class B(u: ()?, n: int)` the
// unit argument sits at source index 1, whose slot is `n: int`, so the rescue
// was asked for a unit placeholder at an `int` formal, returned null, and the
// `if (!v) { baseArgVals.clear(); break; }` edge ABANDONED the entire
// pre-population — both slots kept their calloc zero. The `side` line still
// prints, because `C.Nothing()` is evaluated before the rescue is consulted,
// which is what makes the silent-zero outcome so easy to miss.
//
// Post-fix the rescue asks `fieldForArg(1)`, which is slot 0 (`u: ()?`), and
// the present-unit wrap lands where §22.5 bound it.
// ---------------------------------------------------------------------------

BEZEL_TEST(inheritance_clause_named_arg_binding_4491,
           named_unit_clause_argument_rescued_against_its_bound_slot_4_7) {
    auto r = runAot(
        "module M; "
        "class B(u: ()?, n: int) "
        "  public method Show() "
        "    Console.WriteLine((u <> null).ToString()); "
        "    Console.WriteLine(n.ToString()); "
        "  end Show; "
        "end B; "
        "class D() : B(n := 1, u := C.Nothing()) "
        "  public method Go() "
        "    Show(); "
        "  end Go; "
        "end D; "
        "class C "
        "  public static method Nothing() "
        "    Console.WriteLine(\"side\"); "
        "  end Nothing; "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.Go(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("side\nTrue\n1\n"));
}

// ---------------------------------------------------------------------------
// THE SEMA-SIDE PIN — and the only row that exercises the half of this fix
// that is NOT in codegen.
//
// The same loop in `ExprTC::visitTopDecl` that publishes the binding also
// runs #3750's §22.5 "Defaults and modifiers" check on each clause argument:
// an expression producing a fresh OWNED value of non-trivial-dispose class
// type (§28.9) is admitted only when the base parameter it binds to is
// declared `owned`, and is a compile error on a plain (borrowing) one. That
// check read `basePosParams[ai]` — the slot at the argument's SOURCE
// position — so with named arguments it asked the WRONG parameter's modifier.
//
// Here the clause is `: Base(r := new Res(7), n := 1)` against
// `class Base(n: int, owned r: Res)`. §22.5 binds the producer to `owned r`,
// which is exactly the form the rule ADMITS — but the pre-fix check looked at
// source position 0, whose slot is the plain `n: int`, and rejected a legal
// program. MEASURED on e9239ab4:
//
//   …:10:23: base-call argument 1 produces an owned value of
//   non-trivial-dispose class type, which would have no owner to discharge
//   it; declare the base parameter `owned` to receive it, or use a
//   trivial-dispose default (§22.5)
//
// so this is a FALSE REJECTION pin, not a crash pin — the one direction a
// value-oracle file otherwise has no row for.
// ---------------------------------------------------------------------------

BEZEL_TEST(inheritance_clause_named_arg_binding_4491,
           named_clause_owned_producer_checked_against_its_bound_slot_3750) {
    auto r = runAot(
        "module M; "
        "class Res "
        "  public constructor(id: int) end constructor; "
        "  destructor end destructor; "
        "  public method Id(): int => 1; "
        "end Res; "
        "class Base(n: int, owned r: Res) "
        "  public method Use(): int => r.Id() + n; "
        "end Base; "
        "class D() : Base(r := new Res(7), n := 1) "
        "  public method Go() "
        "    Console.WriteLine(Use().ToString()); "
        "  end Go; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.Go(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n"));
}

// ---------------------------------------------------------------------------
// CONTROL for the row above — the TRUE POSITIVE direction. The same clause
// against `class Base(n: int, r: Res)`, where the bound slot really is plain,
// must still be REJECTED. Pre-fix it was rejected too, but by accident (the
// check happened to be looking at a different plain slot); this row is what
// keeps the re-keying from being mistaken for "stop checking".
// ---------------------------------------------------------------------------

BEZEL_TEST(inheritance_clause_named_arg_binding_4491,
           named_clause_owned_producer_on_a_plain_slot_still_rejected_3750) {
    auto r = runAot(
        "module M; "
        "class Res "
        "  public constructor(id: int) end constructor; "
        "  destructor end destructor; "
        "  public method Id(): int => 1; "
        "end Res; "
        "class Base(n: int, r: Res) "
        "  public method Use(): int => r.Id() + n; "
        "end Base; "
        "class D() : Base(r := new Res(7), n := 1) "
        "  public method Go() "
        "    Console.WriteLine(Use().ToString()); "
        "  end Go; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.Go(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "produces an owned value of non-trivial-dispose class type"));
}

// ---------------------------------------------------------------------------
// CONTROL — named clause arguments written in DECLARED order. Textual order
// and §22.5 order coincide, so this passes on both sides of the fix. It is
// here because a re-keyed store that reversed the map (or that applied the
// binding only to reordered clauses) would break exactly this shape.
// ---------------------------------------------------------------------------

BEZEL_TEST(inheritance_clause_named_arg_binding_4491,
           named_clause_args_in_declared_order_still_bind_correctly_22_5) {
    auto r = runAot(
        "module M; "
        "class B(n: int, s: string) "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class D(v: int) : B(n := v, s := \"y\") "
        "  public method Go() "
        "    Show(); "
        "  end Go; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(2); "
        "    d.Go(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\ny\n"));
}

// ---------------------------------------------------------------------------
// CONTROL — an ALL-POSITIONAL inheritance clause, the pairing case (a) has
// always had. Sema publishes an identity binding for it and the store must be
// bit-identical to the pre-fix one.
// ---------------------------------------------------------------------------

BEZEL_TEST(inheritance_clause_named_arg_binding_4491,
           all_positional_clause_unchanged_22_5) {
    auto r = runAotChecked(
        "module M; "
        "class B(n: int, s: string) "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class D(v: int) : B(v, \"x\") "
        "  public method Go() "
        "    Show(); "
        "  end Go; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(1); "
        "    d.Go(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\nx\n"));
}

// ---------------------------------------------------------------------------
// CONTROL — a clause that binds an EXPLICIT base constructor rather than the
// base's primary parameter list. `emitImplicitBase` lowers and CALLS that
// constructor (#3731/#3742's `baseClauseExplicitCtor`), and case (a) of
// `prePopulateBasePositionalFields` declines the pre-population entirely so
// the arguments are not evaluated twice. Sema publishes no binding for this
// shape — the base declares a constructor of the clause's arity — so the
// guard must be observably undisturbed.
//
// ⚠ This row does NOT claim the explicit-base-ctor arm binds by name: it is
// all-positional precisely so that it pins the GUARD and nothing else. That
// arm pairs `clsBaseArgs[i]` with the constructor's parameter `i` in
// `emitImplicitBase`, which is a third §22.5 site this fix deliberately does
// not touch (see the commit body).
// ---------------------------------------------------------------------------

BEZEL_TEST(inheritance_clause_named_arg_binding_4491,
           positional_clause_to_explicit_base_ctor_still_declined_3742) {
    auto r = runAotChecked(
        "module M; "
        "class B "
        "  public n: int := 0; "
        "  public s: string := \"\"; "
        "  public constructor(a: int, b: string) "
        "    n := a; "
        "    s := b; "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class D(v: int) : B(v, \"e\") "
        "  public method Go() "
        "    Show(); "
        "  end Go; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(4); "
        "    d.Go(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4\ne\n"));
}
