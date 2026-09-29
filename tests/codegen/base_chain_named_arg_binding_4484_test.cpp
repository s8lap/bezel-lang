// Regression tests for issue 4484 — a NAMED-argument `base(...)` call that
// binds the base's PRIMARY parameter list was paired in TEXTUAL order
// (§22.5 "Optional parameters and named arguments" / §22.8 step 0 / §22.10,
// v0.306).
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
// and the section says where the rule reaches: "every construction site in
// the language reaches it through the same binding — a call, a `new`
// expression, and a record construction alike".
//
// WHAT THE COMPILER DID. `Emitter::prePopulateBasePositionalFields`
// (LlvmBackend_Exprs2.cpp) is §22.8 step 0's ONLY lowering site for a base
// whose primary parameter list a `base(...)` call binds — the base has a
// primary constructor and no explicit constructor of the call's arity, so
// `emitChainCall` takes its `!isThis && !hasAnyConstructor` branch and never
// lowers the chain arguments at all. That site's case (b) iterated
// `chainCall->args` positionally and never read `arg.name`, so it paired
// argument `i` with positional field `i` whatever the argument was called.
//
// MEASURED ON 4d9d186a (pre-fix), for
//
//   class B(n: int, s: string) … end B;
//   class D : B
//     public constructor() base(s := "x", n := 1); end constructor;
//   end D;
//
// the emitter stored the `string` pointer into the 4-byte `n` slot and the
// integer `1` into the `string` slot. `new D().Show()` printed `4627856` —
// the low 32 bits of the string pointer read as an `int` — and then died in
// `bz_string_to_utf8z` dereferencing `0x9`; under `--sanitize=address`,
// "SEGV on unknown address 0x9 … READ memory access".
//
// WHY A `decimal` PAYLOAD IS THE ASAN ORACLE. The `string` form above is an
// 8-byte store into a 4-byte slot: ASan's 8-byte fast path reads the shadow
// granule the store STARTS in, which is valid memory, so the store itself is
// silent and only the later dereference faults. A 16-byte `decimal` bound by
// name into a TRAILING `int` slot crosses the end of the allocation and is
// reported at the store:
//
//   class B(d: decimal, n: int) …    base(n := 1, d := 7.5m)
//   ERROR: AddressSanitizer: heap-buffer-overflow … WRITE of size 16 at …
//   11 bytes after 32-byte region
//
// This is the same trap #3562 recorded on the same lines; the ASan row below
// uses the 16-byte shape for exactly that reason.
//
// HOW IT IS FIXED. Not with a second copy of the §22.5 rule at the store.
// Sema publishes the binding for this call through the ONE binder body every
// other construction site already runs (`bindArgsToParams` over
// `sema::FormalsView::of(recordPositionalFieldSymbols(base))`, #3173/#3178) on
// the ONE `SemaContext` side table whose key #3583 widened to
// `const ast::Expr*` precisely so a chain `CallExpr` could be one
// (`ExprTC::publishBaseChainPrimaryBinding`), and the pre-population reads it
// exactly as `storePrimaryCtorPositionalArgs` reads the `new`-site binding.
//
// WHICH ROWS ARE THE PINS. Seven rows fail on the pre-fix compiler:
// `reversed_named_args_*`, `reversed_named_decimal_*`,
// `positional_then_named_out_of_order_*`, `three_cycle_named_permutation_*`,
// `named_args_evaluate_in_textual_order_*`, `named_unit_argument_rescued_*`
// and `omitted_middle_slot_*`. The `declared order`, `all positional` and
// `#3562 convenience` rows agree with textual order by construction and pass
// on BOTH sides — they are here as controls, to catch a "fix" that re-keys
// the store and breaks the pairing that was already right, and to prove the
// guard #3562 put on this same function is undisturbed.
//
// ⚠ THE PERMUTATIONS ARE NOT INTERCHANGEABLE. `[1, 0]` and `[0, 2, 1]` are
// SELF-INVERSE, so a row using either cannot tell a `paramToArg` map from its
// inverse — the most plausible way to get this fix wrong. Only
// `three_cycle_named_permutation_*` (`[1, 2, 0]`) and `omitted_middle_slot_*`
// (`[1, -1, 0]`) have that discriminating power. Do not "simplify" the
// 3-cycle row into another two-argument swap.

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

// ---------------------------------------------------------------------------
// THE PIN (issue body's own repro). `base(s := "x", n := 1)` against
// `class B(n: int, s: string)`. §22.5 binds `s` to the `string` slot and `n`
// to the `int` slot wherever they sit; the pre-fix store paired them by
// position and produced a garbage `int` and a wild `string`.
//
// Asserts the printed VALUES, not just the exit status: the loud→quiet trade
// (a guard that suppresses the store and leaves the object calloc-zero) is
// precisely what an exit-status oracle cannot see, which is the lesson
// #3562's own test file records.
// ---------------------------------------------------------------------------

BEZEL_TEST(base_chain_named_arg_binding_4484,
           reversed_named_args_bind_by_name_not_position_22_5) {
    auto r = runAot(
        "module M; "
        "class B(n: int, s: string) "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class D : B "
        "  public constructor() "
        "    base(s := \"x\", n := 1); "
        "  end constructor; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\nx\n"));
}

// ---------------------------------------------------------------------------
// THE ASAN PIN. A 16-byte `decimal` bound BY NAME into what textual order
// makes the trailing 4-byte `int` slot. `runAotChecked` compiles the
// generated program with AddressSanitizer and flips `ran` to false on any
// sanitizer report, so pre-fix this row fails at `r.ran` with
// "heap-buffer-overflow … WRITE of size 16 … 11 bytes after 32-byte region"
// attached — the store itself, not a later read.
// ---------------------------------------------------------------------------

BEZEL_TEST(base_chain_named_arg_binding_4484,
           reversed_named_decimal_does_not_overflow_trailing_int_slot_22_5) {
    auto r = runAotChecked(
        "module M; "
        "class B(d: decimal, n: int) "
        "  public method Show() "
        "    Console.WriteLine(d.ToString()); "
        "    Console.WriteLine(n.ToString()); "
        "  end Show; "
        "end B; "
        "class D : B "
        "  public constructor() "
        "    base(n := 1, d := 7.5m); "
        "  end constructor; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var x := new D(); "
        "    x.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7.5\n1\n"));
}

// ---------------------------------------------------------------------------
// THE MIXED PIN. §22.5: "Named arguments may appear in any order AFTER
// positional arguments." `base(1, k := 7, s := "m")` against
// `class B(n: int, s: string, k: int)` binds slot 0 positionally and then
// names the last two out of order, so textual order and §22.5 order differ on
// arguments 1 and 2. Pre-fix the integer `7` landed in the `string` slot and
// the string `"m"` in the trailing `int` slot.
// ---------------------------------------------------------------------------

BEZEL_TEST(base_chain_named_arg_binding_4484,
           positional_then_named_out_of_order_binds_by_name_22_5) {
    auto r = runAot(
        "module M; "
        "class B(n: int, s: string, k: int) "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "    Console.WriteLine(k.ToString()); "
        "  end Show; "
        "end B; "
        "class D : B "
        "  public constructor() "
        "    base(1, k := 7, s := \"m\"); "
        "  end constructor; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.Show(); "
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
// Every other reversal row in this file uses a SELF-INVERSE permutation:
// `base(s := …, n := …)` binds `[1, 0]` and `base(1, k := …, s := …)` binds
// `[0, 2, 1]`, and a permutation equal to its own inverse maps to ITSELF under
// a `paramToArg` / `argToParam` confusion. So neither of those rows can detect
// an inverted map — they would pass on a `fieldForArg`/`argForField` pair that
// had the direction backwards, and the object would still be built correctly
// only by accident of the permutation chosen.
//
// `base(c := 3, a := 1, b := 2)` against `class B(a: int, b: int, c: int)`
// binds `paramToArg = [1, 2, 0]`, a 3-cycle whose inverse is `[2, 0, 1]` — a
// DIFFERENT map. Reading the table in the wrong direction therefore produces
// `3 1 2`, which is also exactly what the pre-fix textual pairing produces, so
// this one row pins both the defect and the most plausible mis-fix of it.
//
// Measured: pre-fix `3\n1\n2\n`, post-fix `1\n2\n3\n`.
// ---------------------------------------------------------------------------

BEZEL_TEST(base_chain_named_arg_binding_4484,
           three_cycle_named_permutation_binds_by_name_22_5) {
    auto r = runAot(
        "module M; "
        "class B(a: int, b: int, c: int) "
        "  public method Show() "
        "    Console.WriteLine(a.ToString()); "
        "    Console.WriteLine(b.ToString()); "
        "    Console.WriteLine(c.ToString()); "
        "  end Show; "
        "end B; "
        "class D : B "
        "  public constructor() "
        "    base(c := 3, a := 1, b := 2); "
        "  end constructor; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.Show(); "
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
// argument changes which parameter it binds to, never when it runs
// (`M(b := Next(), a := Next())` evaluates the `b` expression first)."
//
// That sentence is the whole justification for the shape of the fix: the
// argument fill loop stays in SOURCE order and `baseArgVals` stays
// source-indexed, and only the STORE is re-keyed by the §22.5 binding. A
// "simplification" that reordered the lowering into parameter order would keep
// every other row in this file green and silently invert the observable
// side-effect order, so the rule needs its own pin rather than only a comment.
//
// `base(s := Log.B(), n := Log.A())` against `class Base(n: int, s: string)`:
// `Log.B()` is written first, so it runs first (`AtB = 1`) even though it
// binds the SECOND parameter, and `Log.A()` runs second (`AtA = 2`) even
// though it binds the first. The values still land by name (`n = 1`,
// `s = "x"`), which is the point — order and binding are independent.
//
// This is a real pin, not documentation: pre-fix the same program stores the
// string into the `int` slot and SIGSEGVs before it can print anything.
// ---------------------------------------------------------------------------

BEZEL_TEST(base_chain_named_arg_binding_4484,
           named_args_evaluate_in_textual_order_but_bind_by_name_22_5) {
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
        "class D : Base "
        "  public constructor() "
        "    base(s := Log.B(), n := Log.A()); "
        "  end constructor; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.Show(); "
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
// THE §4.7 UNIT-RESCUE PIN. `fieldForArg` is the fix's only other new
// accessor, and the #3502 unit rescue is its only non-defensive caller.
//
// §4.7: a unit-typed argument has NO lowered value — `()` lowers to LLVM
// `void`, so `lowerExpr` returns null — and #3502's rescue materializes the
// §4.7 placeholder for it, reading the TYPE of the positional slot the
// argument lands in. Pre-fix that slot was `basePosFields[baseArgVals.size()]`
// — the argument's SOURCE position. For `base(n := 1, u := C.Nothing())`
// against `class B(u: ()?, n: int)` the unit argument sits at source index 1,
// whose slot is `n: int`, so the rescue was asked for a unit placeholder at an
// `int` formal, returned null, and the `if (!v) { baseArgVals.clear(); break; }`
// edge ABANDONED the entire pre-population — both slots kept their calloc
// zero. Measured pre-fix `side\nFalse\n0\n`; the `side` line still prints
// because `C.Nothing()` is evaluated before the rescue is consulted, which is
// what makes the silent-zero outcome so easy to miss.
//
// Post-fix the rescue asks `fieldForArg(1)`, which is slot 0 (`u: ()?`), and
// the present-unit wrap lands where §22.5 bound it: `side\nTrue\n1\n`.
// ---------------------------------------------------------------------------

BEZEL_TEST(base_chain_named_arg_binding_4484,
           named_unit_argument_rescued_against_its_bound_slot_4_7) {
    auto r = runAot(
        "module M; "
        "class B(u: ()?, n: int) "
        "  public method Show() "
        "    Console.WriteLine((u <> null).ToString()); "
        "    Console.WriteLine(n.ToString()); "
        "  end Show; "
        "end B; "
        "class D : B "
        "  public constructor() "
        "    base(n := 1, u := C.Nothing()); "
        "  end constructor; "
        "end D; "
        "class C "
        "  public static method Nothing() "
        "    Console.WriteLine(\"side\"); "
        "  end Nothing; "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("side\nTrue\n1\n"));
}

// ---------------------------------------------------------------------------
// THE DEFAULT-FILL PIN. §22.5: "Every parameter that declares no default must
// be bound" and an unbound optional parameter takes its default — per SLOT.
// With named arguments the SUPPLIED slots are no longer a prefix:
// `base(m := 9, n := 1)` against `class B(n: int, k: int := 5, m: int := 7)`
// supplies slots 0 and 2 and omits slot 1. (The parameter list keeps §22.5's
// "every parameter after the first defaulted parameter must itself have a
// default" — the non-prefix shape comes from the NAMING, not from an illegal
// declaration.)
//
// This pins the OTHER half of the fix. The §22.5 default loop in
// `prePopulateBasePositionalFields` (#3777) used to start at the watermark
// `min(baseArgVals.size(), basePosFields.size())` — here 2 — so pre-fix it
// stored `9` into `n`, `1` into `k`, and defaulted `m` to `7`: the two
// supplied values in the wrong slots AND the one omitted slot never reached.
// It now asks the §22.5 binding per slot.
// ---------------------------------------------------------------------------

BEZEL_TEST(base_chain_named_arg_binding_4484,
           omitted_middle_slot_takes_its_default_22_5) {
    auto r = runAot(
        "module M; "
        "class B(n: int, k: int := 5, m: int := 7) "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(k.ToString()); "
        "    Console.WriteLine(m.ToString()); "
        "  end Show; "
        "end B; "
        "class D : B "
        "  public constructor() "
        "    base(m := 9, n := 1); "
        "  end constructor; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n5\n9\n"));
}

// ---------------------------------------------------------------------------
// CONTROL — named arguments written in DECLARED order. Textual order and
// §22.5 order coincide, so this passes on both sides of the fix. It is here
// because a re-keyed store that reversed the map (or that only applied the
// binding to reordered calls) would break exactly this shape.
// ---------------------------------------------------------------------------

BEZEL_TEST(base_chain_named_arg_binding_4484,
           named_args_in_declared_order_still_bind_correctly_22_5) {
    auto r = runAot(
        "module M; "
        "class B(n: int, s: string) "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class D : B "
        "  public constructor() "
        "    base(n := 2, s := \"y\"); "
        "  end constructor; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\ny\n"));
}

// ---------------------------------------------------------------------------
// CONTROL — an ALL-POSITIONAL `base(...)`, the pairing this function has
// always had. Sema publishes an identity binding for it and the store must be
// bit-identical to the pre-fix one.
// ---------------------------------------------------------------------------

BEZEL_TEST(base_chain_named_arg_binding_4484,
           all_positional_base_call_unchanged_22_5) {
    auto r = runAotChecked(
        "module M; "
        "class B(n: int, s: string) "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class D : B "
        "  public constructor() "
        "    base(1, \"x\"); "
        "  end constructor; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\nx\n"));
}

// ---------------------------------------------------------------------------
// CONTROL — the path #3562 fixed on this same function, with ALL-POSITIONAL
// arguments. `base(1, 7.5m)` binds B's §22.10 convenience constructor, whose
// parameter list is NOT the primary's; #3562's guard declines the
// pre-population entirely and lets the chained-to primary's own §22.8 step 0
// write both slots. This fix must not re-admit that store, and a §22.5
// binding must not be consulted there at all (sema publishes none, because
// the base declares a constructor of the call's arity).
//
// Mirrors `base_chain_convenience_ctor_blind_pairing_3562`'s own pin, kept
// here so this fix's own file fails if it regresses that one.
// ---------------------------------------------------------------------------

BEZEL_TEST(base_chain_named_arg_binding_4484,
           positional_base_call_to_convenience_ctor_still_declined_3562) {
    auto r = runAotChecked(
        "module M; "
        "class B(x: int, y: int) "
        "  public constructor(a: int, d: decimal) "
        "    this(a, 42); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(x.ToString()); "
        "    Console.WriteLine(y.ToString()); "
        "  end Show; "
        "end B; "
        "class D : B "
        "  public constructor() "
        "    base(1, 7.5m); "
        "  end constructor; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n42\n"));
}
