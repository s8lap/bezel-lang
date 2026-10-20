// fix-forward #6561 (§5.1 "Constant narrowing" + §22.6, v0.338).
//
// THE REGRESSION THIS SUITE PINS. #6554 (branch tip 1c5191ab) relaxed
// narrowIntConstantToTarget's SOURCE gate so the source may be a constant of
// ANY integer type, per §5.1: "The rule is **value-based, not type-based**:
// the source may be a compile-time constant of **any** integer type — not
// only `int`". That reading is correct and is NOT in dispute here. What
// 1c5191ab broke is the overload path's ability to publish the WINNER's
// narrowed type, and it broke it only in ONE declaration order, which is why
// the producer's own 16 rows (all single-order) never saw it.
//
// THE MECHANISM, MEASURED (not inferred) by tracing every
// narrowIntConstantToTarget entry on 1c5191ab. `resolveOverload`'s
// per-candidate grid (ExprTypecheck_Calls.cpp) calls the rule once per
// (candidate, argument) cell, and the call MUTATES a single shared record —
// `ctx_.setExprType` on the argument expression. `argTypes[i]` is ONE vector
// shared by every candidate and is never rewritten per cell, so cell `c`
// grades at whatever width cell `c-1` left behind, and the width that
// SURVIVES the loop is the LAST cell's, not the WINNER's:
//
//   Pick(short) declared FIRST, Pick(byte) second, called `C.Pick(N)`:
//
//     c=0  from=int  to=short  -> re-types, recorded := short
//     c=1  from=short to=byte  -> re-types, recorded := byte
//     §22.6 winner: c=0  (short, graded Exact)
//     recorded type left behind: BYTE      <-- disagrees with the winner
//
// §22.6 then picks the RIGHT candidate. The recorded type is what disagrees,
// and codegen reads the recorded type: `coerceExprValueToSlot`
// (LlvmBackend_Layout.cpp) narrows a call argument to the parameter width ONLY
// when `ctx_.exprType(arg)` IS that width — its gate is "codegen is repairing
// a width its own lowering failed to honour", not "perform a conversion".
// It saw `byte` where the winner wanted `short`, so it declined, and the
// i32-loaded constant reached an i16 parameter:
//
//   %5 = load i32, ptr %4, align 4
//   i16 %6 = call i32 @"bz$M.C$Pick"(i32 %5)
//   bezelc: codegen error: IR verification failed
//
// Note `-c` (sema only) exits 0 on that program: sema resolved, and it was
// codegen that emitted the bad IR. A sema-only row would PASS vacuously.
//
// WHY REVERSE ORDER IS INVISIBLE. With `Pick(byte)` declared first the LAST
// cell is `short` and the WINNER is `byte` — still a disagreement, but in the
// direction codegen's widening direction already repairs, so the program
// compiles and prints 10. The defect therefore only REJECTS in one order.
// No existing fixture had two narrower-int candidates with the wider one
// declared first; that is the blind spot, and it is why the rows below come
// in PAIRS: one order alone cannot distinguish "fixed" from "still broken",
// and an aggregate "8/8 PASS" would hide a row that never failed.
//
// THE SHAPE, NOT THE REPRO. §22.6 "Implicit conversions and tie-breaking":
// "two candidates that both require an implicit conversion at the same
// position are **equally ranked** there, regardless of which conversion each
// uses" — and "An exact match is the only relation that distinguishes one
// applicable candidate from another". So for a `const N: int := 5` argument
// against `Pick(short)` / `Pick(byte)`, NEITHER candidate is an exact match:
// both cells convert, so the two are co-equal and the first-declared wins.
// That order-dependence in the SELECTION is pre-existing (#2985's own comment
// at ExprTypecheck_Calls.cpp:3249 records it as an implementation choice, and
// the spec does not settle it) and is deliberately NOT what these rows pin.
//
// What must hold is the weaker, decidable property: whichever candidate §22.6
// selects, the program must COMPILE, RUN, and PRINT that candidate's answer —
// the recorded type and the winning signature must never disagree. Each row
// below therefore asserts the PRINTED VALUE, not just `r.ran`: a fix that
// picked the wrong overload, or emitted a stale width, is caught by the value.
//
// MEASURED equivalence with the base: on 26ca3bd9 (before #6554) and on this
// fix, every program below compiles, runs and prints the SAME value — the
// selection is unchanged, and only the malformed-IR rejection is removed. The
// rows are a fix-forward's "no behaviour flip" evidence, not a new ranking.

#include <string>

#include "support/aot_support.hpp"  // runAot
#include "test_harness.hpp"

using namespace bezel::test;

namespace {

// One static `Pick` per entry of `sigs`, in DECLARATION order, each returning
// its 1-based position so the PRINTED VALUE identifies which candidate §22.6
// selected. `sigs` order is the variable under test: the same call site and
// the same argument (`const N: int := 5`), only the declaration order differs.
std::string pickProg(const char* module, const std::string& sigs,
                     const char* call, const char* decl = "const N: int := 5;") {
    std::string s = std::string("module ") + module + ";\npublic class C\n";
    int n = 1;
    size_t pos = 0;
    for (;;) {
        size_t comma = sigs.find(',', pos);
        std::string t = sigs.substr(pos, comma == std::string::npos
                                               ? std::string::npos
                                               : comma - pos);
        s += "  public static method Pick(x: " + t + "): int => " +
             std::to_string(n++) + ";\n";
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    s += "  public static method Main(): int\n";
    s += std::string("    ") + decl + "\n";
    s += std::string("    ") + call + "\n";
    s += "    return 0;\n  end Main;\nend C;\n";
    return s;
}

// stdout of a program that must compile, run and exit 0. `ran == false` and a
// non-zero exit both read back as markers no expectation equals, so the
// malformed-IR REJECTION FAILS an accept row rather than passing it vacuously
// — which is the whole point: on 1c5191ab these rows must go red.
std::string prints(const std::string& src) {
    auto r = runAot(src);
    if (!r.ran) return "<did not run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

}  // namespace

// ── The issue's own repro, in BOTH orders (the pair is the test) ───────────
//
// `const N: int := 5` is an `int`-SOURCED constant. That matters: the shape
// is not confined to the wide-source population #6554 opened, so a fix that
// only handled wide sources would leave every row below red.

BEZEL_TEST(overload_narrow_order_6561,
           two_narrower_int_candidates_wider_declared_first_spec_5_1) {
    // `short` declared FIRST. On 1c5191ab: "IR verification failed".
    BEZEL_EXPECT_EQ(prints(pickProg("M6561a", "short,byte",
                                    "Console.WriteLine($\"{C.Pick(N)}\");")),
                    "1\n");
}

BEZEL_TEST(overload_narrow_order_6561,
           two_narrower_int_candidates_narrower_declared_first_spec_5_1) {
    // The mirror. On 1c5191ab this ALREADY passed (prints 1) — it is the
    // control arm that proves the pair isolates DECLARATION ORDER rather
    // than the shape, and it must stay green.
    BEZEL_EXPECT_EQ(prints(pickProg("M6561b", "byte,short",
                                    "Console.WriteLine($\"{C.Pick(N)}\");")),
                    "1\n");
}

// ── Three candidates: the same disagreement, one more cell to lose it in ──
//
// On 1c5191ab every order whose FIRST-declared candidate is `short` rejects
// with malformed IR, because `short` wins (first-declared, all cells
// converting) and the LAST cell leaves `byte` recorded. The two rows whose
// first declaration is `byte` are the controls.

BEZEL_TEST(overload_narrow_order_6561,
           trio_wider_candidate_first_short_byte_int_spec_5_1) {
    BEZEL_EXPECT_EQ(prints(pickProg("M6561c", "short,byte,int",
                                    "Console.WriteLine($\"{C.Pick(N)}\");")),
                    "1\n");
}

BEZEL_TEST(overload_narrow_order_6561,
           trio_wider_candidate_first_short_int_byte_spec_5_1) {
    BEZEL_EXPECT_EQ(prints(pickProg("M6561d", "short,int,byte",
                                    "Console.WriteLine($\"{C.Pick(N)}\");")),
                    "1\n");
}

BEZEL_TEST(overload_narrow_order_6561,
           trio_narrower_candidate_first_byte_short_int_control_spec_5_1) {
    BEZEL_EXPECT_EQ(prints(pickProg("M6561e", "byte,short,int",
                                    "Console.WriteLine($\"{C.Pick(N)}\");")),
                    "1\n");
}

BEZEL_TEST(overload_narrow_order_6561,
           trio_int_first_control_spec_5_1) {
    // `int` needs no §5.1 narrowing at all (it IS the argument's type), so
    // this cell is never a re-type and cannot participate in the defect.
    BEZEL_EXPECT_EQ(prints(pickProg("M6561f", "int,short,byte",
                                    "Console.WriteLine($\"{C.Pick(N)}\");")),
                    "1\n");
}

// ── §22.6 Example 1's `int`/`long` pair, both orders: the CONTROL that
//    #6554's Convert cap exists for. The argument is a `long`-SOURCED
//    constant (`const N: long := 42;`, §22.6 Example 1's `Log(42L)`), so
//    `long` is the argument's OWN type — the one EXACT match — and must win
//    in EITHER declaration order. ───────────────────────────────────────────
//
//    These two are CONTROLS: green on 26ca3bd9, on 1c5191ab and here. They
//    are in the suite because a repair that published the winner's narrowing
//    could break the order-independence #6554 bought, and only a pair in
//    both orders can detect that. The printed value differs between the two
//    (the `long` overload is position 1 in one and position 2 in the other);
//    what matters is that BOTH select the `long` overload.

BEZEL_TEST(overload_narrow_order_6561,
           int_long_pair_long_declared_first_spec_22_6) {
    BEZEL_EXPECT_EQ(prints(pickProg("M6561g", "int,long",
                                    "Console.WriteLine($\"{C.Pick(N)}\");",
                                    "const N: long := 42;")),
                    "2\n");
}

BEZEL_TEST(overload_narrow_order_6561,
           int_long_pair_int_declared_first_spec_22_6) {
    BEZEL_EXPECT_EQ(prints(pickProg("M6561h", "long,int",
                                    "Console.WriteLine($\"{C.Pick(N)}\");",
                                    "const N: long := 42;")),
                    "1\n");
}
