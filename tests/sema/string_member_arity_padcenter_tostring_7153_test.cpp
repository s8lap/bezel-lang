// fix #7153 — two §9.6-declared `string` instance members are MISSING from
// the built-in `string` arity table the #7140 fix introduced
// (`builtinStringInstanceArities()` in `src/sema/ExprTypecheck_Calls.cpp`),
// so a call to either at a WRONG ARITY is ACCEPTED and the surplus argument
// is SILENTLY DROPPED while the program prints a plausible wrong value.
//
// WHAT THE DOCUMENTS REQUIRE.
//
//   * stdlib §9.6 (v0.343), the `String` class declaration, Modification group:
//         public pure method PadLeft(totalWidth: int, paddingChar: char := ' '): string;
//         public pure method PadRight(totalWidth: int, paddingChar: char := ' '): string;
//         public pure method PadCenter(totalWidth: int, paddingChar: char := ' '): string;
//                     // Pad contract (all three Pad members): totalWidth <= Length returns the original string
//                     // unchanged; totalWidth < 0 raises ArgumentOutOfRangeException.
//                     // PadCenter splits the padding evenly; when it is odd, the extra
//                     // character goes to the RIGHT: "ab".PadCenter(5) = " ab  ".
//     ⇒ `PadCenter` has arity {1, 2}: one REQUIRED parameter and one DEFAULTED
//     one — exactly like its two siblings, which the table already carried.
//   * stdlib §9.6 (v0.343), the Object overrides group:
//         // Object overrides (Equals is the own-typed member above)
//         public pure override method GetHashCode(): int;   // content-based, agrees with Equals
//         public pure override method ToString(): string;   // returns this
//     ⇒ `ToString` has arity {0} on `string`. It is the `Object`-override
//     spelling, which is WHY the widening pass over the member list missed it.
//
// THE DEFECT. A built-in `string` member is a compiler INTRINSIC: it has no
// MethodSymbol, so §22.5's argument-count check never ran for it, and the
// member was typed by NAME alone. The lowerings (`Emitter::tryLowerStringMethod`
// and the static-factory arms) key on the member name AND the argument count,
// so a call at an arity no §9.6 overload declares matches NO lowering.
//
// MEASURED pre-fix (base 3c2ef83e, the tree this lane starts from; the
// programs are the probe files left in
// /var/tmp/bezel-scratch/r35-7153/probes/):
//
//   "ab".PadCenter()            → ACCEPTED with only a warning; the generated
//                                  program then died with SIGSEGV on address
//                                  0x8 (the zero-initialised slot read as a
//                                  string header)
//   "ab".PadCenter(5, '-', 7)   → ACCEPTED; printed "[-ab--]" — the surplus
//                                  `7` was DROPPED
//   "ab".ToString(1)            → ACCEPTED; printed "[ab]" — the surplus `1`
//                                  was DROPPED
//   "ab".PadLeft(5, 'x', 'y')   → REJECTED with the §9.6 arity diagnostic
//                                  (the CONTROL: the sibling IS in the table)
//
//   "ab".PadCenter(5)           → ACCEPTED; printed "[ ab  ]" ✔ (the §9.6
//                                  contract), exit 0, no leak
//   "ab".PadCenter(5, 'x')      → ACCEPTED; printed "[xabxx]" ✔, exit 0, no leak
//   "ab".ToString()             → ACCEPTED; printed "[ab]" ✔, exit 0
//
// ⇒ ⇒ At the VALID arities there is no wrong value and no leak ⇒ the defect is
// exactly the #7140 shape: a wrong arity is ACCEPTED instead of REJECTED.
//
// THE FIX GATES THE ARITY, NEVER THE MEMBER. `"ab".ToString()` is the §9.6
// contract ("returns this") and must keep working; `"ab".PadCenter(5)` and
// `"ab".PadCenter(5, 'x')` are the §9.6 declarations and must keep working.
// The rows below assert a VALUE for each of them, so a fix that gated the
// MEMBER (rejecting the member outright) would fail here.
//
// THE STATIC AND `?.` SPELLINGS ARE NOT TOUCHED. `string.PadCenter(5)` is not
// a §9.6 static member at all (the static surface is a separate table, and
// the static spelling keeps its §3.4 rejection); `s?.PadCenter` is rejected by
// the `?.` lowering gate, and the null-conditional spelling is excluded from
// this arity gate so that ONE mistake is never reported TWICE.
//
// THE CONTROLS ARE THE POINT. This is a gate ADDED to a member, so the live
// risk is over-rejection of the arities §9.6 *does* declare. Every control
// asserts a VALUE, never "it compiled".
//
// (A SEPARATE PRE-EXISTING DEFECT, REPORTED NOT FIXED: the §9.6 Pad contract's
// `totalWidth < 0` half is not enforced for the SIBLING members `PadLeft` /
// `PadRight` — they answer a negative totalWidth with the receiver unchanged.
// #7120 records that; it is NOT this fix's.)

#include <string>

#include "support/aot_result_asserts.hpp"  // diagnosticMessageCount /
                                          // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot — the AOT oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// A bare `module M; class C ... end C;` shell — the string members are
// compiler-intrinsic, so no imports or hierarchy are required.
std::string prog(const std::string& mainBody) {
    return "module M;\nclass C\n  public static method Main(): int\n" +
           mainBody + "\n  end Main;\nend C;\n";
}

// The §9.6 arity diagnostic for `PadCenter`, spelled the way the fix spells
// it: the arity supplied, the §9.6 declaration, and the arities that
// declaration admits.
const char* kPadCenterArityDiag =
    "the call to `PadCenter` on `string` supplies 3 argument(s) but stdlib "
    "§9.6 declares `PadCenter(totalWidth: int, paddingChar: char := ' '): "
    "string` — 1 or 2 argument(s) (§22.5; §9.6)";

// The §9.6 arity diagnostic for `ToString` — the `Object`-override spelling.
const char* kToStringArityDiag =
    "the call to `ToString` on `string` supplies 1 argument(s) but stdlib "
    "§9.6 declares `ToString(): string` — exactly 0 argument(s) (§22.5; "
    "§9.6)";

}  // namespace

// ===========================================================================
// (A) REJECT — `PadCenter` at THREE arguments.
//
// §9.6 declares `PadCenter(totalWidth: int, paddingChar: char := ' ')`: one
// required parameter and one DEFAULTED one ⇒ arities {1, 2}. The three-argument
// shape matches no §9.6 declaration.
//
// MEASURED pre-fix: ACCEPTED, printed "[-ab--]" — the surplus argument was
// DROPPED and a plausible value came out.
//
// The row asserts EXACTLY ONE diagnostic: the arity gate must report the one
// mistake once, not twice (the sibling `PadLeft` gate reports its own mistake
// once, and a double-report would be a defect of its own).
// ===========================================================================
BEZEL_TEST(sema_string_member_arity_padcenter_tostring_7153,
           reject_instance_padcenter_three_arguments_stdlib_9_6) {
    JitResult r = runAot(prog(
        "    var s := \"ab\".PadCenter(5, '-', 7);\n"
        "    Console.WriteLine(\"[\" + s + \"]\");\n"
        "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    // A REAL compile-time rejection — not a crash, not a timeout, not an ASan
    // report (see tests/support/aot_result_asserts.hpp).
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kPadCenterArityDiag));
    // EXACTLY ONE diagnostic for the one mistake.
    BEZEL_EXPECT_EQ(bezel::test::diagnosticMessageCount(r, kPadCenterArityDiag), 1);
}

// ===========================================================================
// (B) REJECT — `PadCenter` at ZERO arguments.
//
// The TOO-LOW direction: §9.6 declares no nullary `PadCenter`.
//
// MEASURED pre-fix: ACCEPTED with only a warning, and the generated program
// then died with SIGSEGV on address 0x8 (the #2477 permissive-acceptance path
// degraded the slot to a zero-initialised value and the program's first read
// of it faulted).
// ===========================================================================
BEZEL_TEST(sema_string_member_arity_padcenter_tostring_7153,
           reject_instance_padcenter_zero_arguments_stdlib_9_6) {
    JitResult r = runAot(prog(
        "    var s := \"ab\".PadCenter();\n"
        "    Console.WriteLine(\"[\" + s + \"]\");\n"
        "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r,
        "the call to `PadCenter` on `string` supplies 0 argument(s) but stdlib "
        "§9.6 declares `PadCenter(totalWidth: int, paddingChar: char := ' '): "
        "string` — 1 or 2 argument(s) (§22.5; §9.6)"));
    BEZEL_EXPECT_EQ(bezel::test::diagnosticMessageCount(r, "the call to `PadCenter` on `string`"), 1);
}

// ===========================================================================
// (C) CONTROL — `PadCenter` at ONE argument keeps its §9.6 VALUE.
//
// §9.6 (v0.343), verbatim from the document's own comment: "PadCenter splits
// the padding evenly; when it is odd, the extra character goes to the RIGHT:
// \"ab\".PadCenter(5) = \" ab  \"."
//
// The value is bracketed so the whitespace-significant padding is unambiguous.
// A fix that gated the MEMBER (rather than the arity) would fail this row.
// ===========================================================================
BEZEL_TEST(sema_string_member_arity_padcenter_tostring_7153,
           accept_instance_padcenter_one_argument_stdlib_9_6) {
    JitResult r = runAot(prog(
        "    Console.WriteLine(\"[\" + \"ab\".PadCenter(5) + \"]\");\n"
        "    return 0;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "[ ab  ]\n");
}

// ===========================================================================
// (D) CONTROL — `PadCenter` at TWO arguments keeps its §9.6 VALUE.
//
// The two-argument form is the DEFAULTED-parameter form
// (`paddingChar: char := ' '`), which is why the arity set is {1, 2} and not
// {2}: a table that admitted only {2} would over-reject the one-argument
// form, and a table that admitted only {1} would over-reject this one.
// ===========================================================================
BEZEL_TEST(sema_string_member_arity_padcenter_tostring_7153,
           accept_instance_padcenter_two_arguments_stdlib_9_6) {
    JitResult r = runAot(prog(
        "    Console.WriteLine(\"[\" + \"ab\".PadCenter(5, 'x') + \"]\");\n"
        "    return 0;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "[xabxx]\n");
}

// ===========================================================================
// (E) REJECT — `ToString` at ONE argument.
//
// §9.6 declares, in the Object overrides group, `public pure override method
// ToString(): string;` — the `Object`-override spelling, at arity {0}. The
// one-argument shape matches no §9.6 declaration.
//
// MEASURED pre-fix: ACCEPTED with NO diagnostic at all, and printed "[ab]" —
// the surplus argument was silently DROPPED.
// ===========================================================================
BEZEL_TEST(sema_string_member_arity_padcenter_tostring_7153,
           reject_instance_tostring_one_argument_stdlib_9_6) {
    JitResult r = runAot(prog(
        "    var s := \"ab\".ToString(1);\n"
        "    Console.WriteLine(\"[\" + s + \"]\");\n"
        "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kToStringArityDiag));
    BEZEL_EXPECT_EQ(bezel::test::diagnosticMessageCount(r, kToStringArityDiag), 1);
}

// ===========================================================================
// (F) CONTROL — `ToString` at ZERO arguments keeps its §9.6 VALUE.
//
// §9.6 (v0.343): `public pure override method ToString(): string;  // returns
// this`. The fix gates the ARITY, never the member: `"ab".ToString()` must
// keep working, and must keep returning the receiver.
// ===========================================================================
BEZEL_TEST(sema_string_member_arity_padcenter_tostring_7153,
           accept_instance_tostring_zero_arguments_stdlib_9_6) {
    JitResult r = runAot(prog(
        "    Console.WriteLine(\"[\" + \"ab\".ToString() + \"]\");\n"
        "    return 0;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "[ab]\n");
}

// ===========================================================================
// (G) CONTROL — the SIBLINGS are untouched.
//
// `PadLeft` and `PadRight` were already in the arity table (the #7140 pick),
// and they must keep being rejected at the same wrong arities — the fix adds
// two rows, it does not re-shape the table.
//
// The rows below use the SAME diagnostic spelling as the #7140 file's own
// rows (`tests/sema/string_member_wrong_arity_rejected_7140_test.cpp`), so the
// two files agree on one wording.
// ===========================================================================
BEZEL_TEST(sema_string_member_arity_padcenter_tostring_7153,
           accept_sibling_pad_members_keep_their_arity_gate_stdlib_9_6) {
    JitResult r = runAot(prog(
        "    var s := \"ab\".PadLeft(5, 'x', 'y');\n"
        "    Console.WriteLine(\"[\" + s + \"]\");\n"
        "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r,
        "the call to `PadLeft` on `string` supplies 3 argument(s) but stdlib "
        "§9.6 declares `PadLeft(totalWidth: int, paddingChar: char := ' '): "
        "string` — 1 or 2 argument(s) (§22.5; §9.6)"));

    JitResult r2 = runAot(prog(
        "    var s := \"ab\".PadRight(5, 'x', 'y');\n"
        "    Console.WriteLine(\"[\" + s + \"]\");\n"
        "    return 0;"));
    BEZEL_EXPECT_FALSE(r2.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r2));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r2,
        "the call to `PadRight` on `string` supplies 3 argument(s) but stdlib "
        "§9.6 declares `PadRight(totalWidth: int, paddingChar: char := ' '): "
        "string` — 1 or 2 argument(s) (§22.5; §9.6)"));
}

// ===========================================================================
// (H) CONTROL — the STATIC and `?.` spellings are NOT gated by this fix.
//
//   * `string.PadCenter(5)` — §9.6 declares no STATIC `PadCenter`, so the
//     static spelling keeps its §3.4 rejection ("type 'string' has no static
//     method or property 'PadCenter'"). The static surface is a SEPARATE
//     table; this fix adds a row to the INSTANCE table only.
//   * `s?.PadCenter(5)` — the null-conditional spelling is excluded from the
//     arity gate (the `?.` lowering gate already rejects it), so ONE mistake
//     is never reported TWICE.
// ===========================================================================
BEZEL_TEST(sema_string_member_arity_padcenter_tostring_7153,
           accept_static_and_null_conditional_spellings_keep_their_own_diagnostics) {
    JitResult r = runAot(prog(
        "    var s := string.PadCenter(5);\n"
        "    Console.WriteLine(\"[\" + s + \"]\");\n"
        "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "has no static method or property 'PadCenter'"));

    JitResult r2 = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var s: string? := \"ab\";\n"
        "    var t := s?.PadCenter(5);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r2.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r2));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r2, "string member `?.PadCenter` is not yet supported in codegen"));
}
