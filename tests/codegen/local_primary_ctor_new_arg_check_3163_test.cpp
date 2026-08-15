// fix #3163 (§22.10 "primary constructors" / §22.5 "Optional parameters and
// named arguments" / §5.1 "Implicit conversions" / §14.5 "Reference arguments";
// spec v0.254) — `new C(args)` against a LOCAL primary-constructor class must
// be §22.5/§5.1-checked, not silently accepted.
//
// ── WHAT WAS BROKEN ────────────────────────────────────────────────────────
//
// ExprTC::checkNewExprArgs (src/sema/ExprTypecheck_Calls.cpp) ran the §22.5/§5.1
// check over `recordPositionalFieldSymbols(t)` only when `t->kind == Record` —
// the CLASS arm of `if (t->hasPositionalFields)` returned without checking.
// The deferral was unconditional: even an unchained
// `class PBox(V: int, W: string)` got no arity check and no per-argument type
// check. The defects measured at this commit:
//
//   (a) WRONG-TYPED ARGUMENT  →  SIGSEGV.  `new PBox(3, 4)` against
//       `class PBox(V: int, W: string)` compiles clean, the integer 4 is
//       stored into the `W` slot (which is a pointer-sized field), and the
//       first read of `W.Length` dereferences a small integer. Exit 139.
//
//   (b) WRONG ARITY (too few) →  SILENT ZERO OBJECT.  `new PBox(3)` against
//       `class PBox(V: int, W: int)` compiles clean and prints `sum=3` — the
//       supplied `V` is stored, but `W` is left at its calloc-zero default
//       because the codegen store loop's old `args.size() == posFields.size()`
//       guard (#3202 already loosened this; the SEMANTIC defect that the
//       missing parameter is silently absorbed is the local twin of #3141's
//       imported-class defect, and this issue). Pre-#3202 the WHOLE object
//       was zero (silent zero object). Post-#3202 the supplied slots still
//       land but the missing slot is zero — a quiet wrong value.
//
//   (c) NO ARGUMENTS → SILENT ZERO OBJECT.  `new PBox()` against
//       `class PBox(V: int, W: string)` compiles clean and reads garbage
//       from V/W (both zero).
//
//   (d) NAMED ARGUMENTS  →  silent zero object (codegen
//       `storePrimaryCtorPositionalArgs` lowers only the all-positional
//       form; a named call would compile clean and leave the object
//       calloc-zero). Rejected loudly so the silent-wrong-object outcome
//       is structurally impossible.
//
// ── THE FIX (one-line summary) ─────────────────────────────────────────────
//
// ExprTC::checkNewExprArgs now runs `checkPositionalNewArgsFromSymbols` (the
// same check the imported arm got in #3141 / #3162) on a LOCAL primary-ctor
// class when no explicit constructor has matching arity. The chained-body-
// constructor interaction (`this(...)` delegation, #1087) is preserved by
// the matching-arity predicate — when an explicit ctor with matching arity
// exists, control falls through to the constructor-overload arm, which is
// what already routes `new C(5)` on `class C(x: int) +
// constructor() this(99)` to the primary ctor path today.
//
// ── ORACLES ────────────────────────────────────────────────────────────────
//
// T1-T3 are positive (compile + run; exit-code checks). T4-T7 are negative
// (sema REJECT, asserted by `hasErrors()` AND a substring on the diagnostic
// message — a coarse "did it reject" check would happily accept a
// neighbouring false reject, so the substring pins which rule fired).
//
// The negative rows go through runAot so the same driver the lane relies on
// is the one driving the assertion — the gate is sharded-vs-serial, NOT
// `--full`-vs-plain (#2633), and this test runs under both oracles.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Run the program; return the exit code on a clean run, -999 if the compile
// was REJECTED (mirrors runJit's failWith semantics so the positive rows can
// distinguish "wrong answer" from "compile failed").
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// True iff the program was REJECTED at compile/link. A coarse predicate — the
// negative rows also assert on the diagnostic substring so a neighbouring
// false reject cannot pass.
bool rejected(const std::string& prog) {
    auto r = runAot(prog);
    return r.diagnostics.hasErrors();
}

// Pin a SPECIFIC diagnostic substring so a coarse "rejected" check cannot
// accept a different reject (e.g. a syntax error in the test program itself
// masquerading as the rule under test).
bool rejectedWith(const std::string& prog, const std::string& needle) {
    auto r = runAot(prog);
    if (!r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (std::string(d.message).find(needle) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

// ===========================================================================
// T1 — §22.10 / §22.5: a §22.10 class with a primary constructor, supplied
// with the correct argument list, stores every slot. Pre-fix this also
// worked (the codegen store loop ran); the row pins that the fix does not
// disturb the happy path.
//
// Sum() = V + W = 3 + 4 = 7. With `class PBox(V: int, W: int)`, this is the
// shape every other row diverges from.
// ===========================================================================
BEZEL_TEST(local_primary_ctor_new_arg_check_3163,
           correct_arity_and_types_still_store_spec_22_10) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "public class PBox(V: int, W: int)"
        "  public method Sum(): int return V + W; end Sum;"
        "end PBox;"
        "class C public static method Main(): int"
        "  using var b := new PBox(3, 4);"
        "  return b.Sum();"
        "end Main; end C;"), 7);
}

// ===========================================================================
// T2 — §22.5: TOO FEW arguments is rejected (was: silent zero object).
//
// `new PBox(3)` for `class PBox(V: int, W: int)`: pre-fix compiled clean and
// printed `sum=3` (V=3, W=calloc-zero). Post-fix the missing `W` is reported
// by the §22.5 check.
// ===========================================================================
BEZEL_TEST(local_primary_ctor_new_arg_check_3163,
           too_few_positional_arguments_rejected_spec_22_5) {
    BEZEL_EXPECT_TRUE(rejected(
        "module M;"
        "public class PBox(V: int, W: int)"
        "  public method Sum(): int return V + W; end Sum;"
        "end PBox;"
        "class C public static method Main(): int"
        "  using var b := new PBox(3);"
        "  return b.Sum();"
        "end Main; end C;"));
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M;"
        "public class PBox(V: int, W: int)"
        "  public method Sum(): int return V + W; end Sum;"
        "end PBox;"
        "class C public static method Main(): int"
        "  using var b := new PBox(3);"
        "  return b.Sum();"
        "end Main; end C;",
        "required parameter `W` is not supplied"));
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M;"
        "public class PBox(V: int, W: int)"
        "  public method Sum(): int return V + W; end Sum;"
        "end PBox;"
        "class C public static method Main(): int"
        "  using var b := new PBox(3);"
        "  return b.Sum();"
        "end Main; end C;",
        "\xC2\xA7""22.5"));
}

// ===========================================================================
// T3 — §5.1: WRONG-TYPED argument is rejected (was: SIGSEGV).
//
// `new PBox(3, 4)` for `class PBox(V: int, W: string)` — `4` is an `int`,
// `W` is a `string`. Pre-fix the int was stored into a pointer-sized field
// and the first read of `W.Length` SIGSEGV'd (exit 139). Post-fix the §5.1
// check catches the assignability before codegen.
//
// The exit code is a discriminator: pre-fix the program SIGSEGV'd at 139;
// post-fix the COMPILE fails before any program is produced. So the same
// exit-code row that pins "the SIGSEGV is gone" is also "the compile is
// rejected" — both checked.
// ===========================================================================
BEZEL_TEST(local_primary_ctor_new_arg_check_3163,
           wrong_argument_type_rejected_spec_5_1) {
    const std::string src =
        "module M;"
        "public class PBox(V: int, W: string)"
        "  public method Len(): int return W.Length; end Len;"
        "end PBox;"
        "class C public static method Main(): int"
        "  using var b := new PBox(3, 4);"
        "  return b.Len();"
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(rejected(src));
    BEZEL_EXPECT_TRUE(rejectedWith(src, "not implicitly convertible"));
    BEZEL_EXPECT_TRUE(rejectedWith(src, "\xC2\xA7""5.1"));
    // And the SIGSEGV exit code (139) is structurally impossible: a rejected
    // compile produces -999, never 139. Pin it so a future "we'll let it
    // through and catch the bad store at runtime" regression cannot pass.
    BEZEL_EXPECT_FALSE(run(src) == 139);
}

// ===========================================================================
// T4 — §22.5: NO ARGUMENTS at all is rejected.
//
// `new PBox()` for `class PBox(V: int, W: string)`: pre-fix compiled clean
// and produced a zero object (every field calloc-zero). Post-fix the §22.5
// check reports the first missing required parameter.
// ===========================================================================
BEZEL_TEST(local_primary_ctor_new_arg_check_3163,
           zero_arguments_rejected_spec_22_5) {
    const std::string src =
        "module M;"
        "public class PBox(V: int, W: string)"
        "  public method Sum(): int return V + W; end Sum;"
        "end PBox;"
        "class C public static method Main(): int"
        "  using var b := new PBox();"
        "  return b.Sum();"
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(rejected(src));
    BEZEL_EXPECT_TRUE(rejectedWith(src, "required parameter `V` is not "
                                         "supplied"));
    BEZEL_EXPECT_TRUE(rejectedWith(src, "\xC2\xA7""22.5"));
}

// ===========================================================================
// T5 — §22.5: a NAMED argument is rejected loudly (was: silent zero object).
//
// `new PBox(W := "abc", V := 3)` for `class PBox(V: int, W: string)`:
// pre-fix the sema check was skipped, codegen
// `storePrimaryCtorPositionalArgs` is all-positional-only so the named call
// fell through the store loop and left the object calloc-zero. Post-fix the
// named shape is rejected with its own diagnostic, naming the rule.
//
// (The LOCAL twin of #3162 T10's imported-class decline — same defect class,
// single-module this time.)
// ===========================================================================
// T5 — §22.5: a NAMED argument to a LOCAL primary-ctor class now lowers
// (was the loud anyNamed decline #3163 added; #3196 supersedes by publishing
// the §22.5 binding to codegen via the SemaContext side table).
//
// `new PBox(W := 4, V := 3)` on `class PBox(V: int, W: int)` was REJECTED
// pre-#3196 (the loud decline). #3196 lowers it and answers `Sum() = 7`.
// This is the LOCAL twin of `xmod_named_arg_imported_primary_ctor_3196.t1_l`
// (one module, no cross-module side); the two rows together pin the
// symmetric behavior on both sides of the module boundary.
// ===========================================================================
BEZEL_TEST(local_primary_ctor_new_arg_check_3163,
           named_argument_local_constructor_lowers_22_5) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "public class PBox(V: int, W: int)"
        "  public method Sum(): int return V + W; end Sum;"
        "end PBox;"
        "class C public static method Main(): int"
        "  using var b := new PBox(W := 4, V := 3);"
        "  return b.Sum();"
        "end Main; end C;"), 7);
}

// ===========================================================================
// T6 — §22.10 / §22.8 step 0: a §22.10 class with a DEFAULTED primary
// parameter, supplied with the full list, stores every slot — the
// happy-path twin of #3202's T1, kept here to pin that the new check does
// not disturb a defaulted-parameter class.
//
// Sum() = A + B = 1 + 2 = 3.
// ===========================================================================
BEZEL_TEST(local_primary_ctor_new_arg_check_3163,
           defaulted_full_arg_list_still_writes_every_slot_spec_22_10) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "public class DefBox(A: int, B: int := 9)"
        "  public method Total(): int return A + B; end Total;"
        "end DefBox;"
        "class C public static method Main(): int"
        "  using var d := new DefBox(1, 2);"
        "  return d.Total();"
        "end Main; end C;"), 3);
}

// ===========================================================================
// T7 — §22.10 chained-body-constructor (this(...)) interaction is preserved
// (#1087). A class with BOTH a primary ctor and an explicit 0-arg ctor that
// delegates to primary via `this(99)` — `new C()` calls the explicit ctor
// (which chains to primary with arg 99). The fix's matching-arity predicate
// routes `new C()` to the constructor-overload arm and DOES NOT run the
// primary check, so this row is the over-rejection control.
//
// GetX() + _f = 99 + 42 = 141 (8-bit truncation: 141). The 0-arg explicit
// ctor is the candidate; primary is reached via `this(99)`.
// ===========================================================================
BEZEL_TEST(local_primary_ctor_new_arg_check_3163,
           chained_explicit_ctor_still_resolves_spec_22_10) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "class C(x: int)"
        "  public _f: int := 42;"
        "  public method GetX(): int => x;"
        "  public constructor() this(99); end constructor;"
        "end C;"
        "class D public static method Main(): int"
        "  using var c := new C();"
        "  return c.GetX() + c._f;"
        "end Main; end D;"), 141);
}
