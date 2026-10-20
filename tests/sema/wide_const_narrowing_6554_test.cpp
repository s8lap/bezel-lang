// fix #6554 (§5.1 "Constant narrowing", v0.338; §17.14, v0.338).
//
// THE RULE, verbatim (spec §5.1, v0.338, line 646):
//
//   "**Constant narrowing.** An expression of integer type that is a
//    **compile-time constant** ([§17.14]) implicitly converts to any integer
//    type whose range contains its value … The rule is **value-based, not
//    type-based**: the source may be a compile-time constant of **any**
//    integer type — not only `int` — and the target any integer type whose
//    range contains the value, **across signedness**
//    (`const N: long := 8; var b: byte := N;` is well-typed; a `uint`-typed
//    constant `7` is admissible where `int` is expected); for non-constant
//    expressions the conversion table and the never-implicit list above govern
//    unchanged. A constant whose value does not fit the target's range is a
//    **compile error** at the conversion site — never a silent truncation."
//
// Two properties of that sentence the pre-fix gate denied. It read the
// "not only `int`" clause as "only `int`", and it read "across signedness" as
// forbidden. Both spec-quoted shapes below were REJECTED on 26ca3bd9 at a
// plain local initializer with no collection in the program:
//
//   const N: long := 8;   var b: byte := N;
//     -> "the initializer for `b` has a type that is not implicitly
//         convertible to the target type (§5.1): between signed and unsigned
//         types of the same or narrower width is never implicit"
//   const U: uint := 7;   var b: int := U;
//     -> same message
//
// and the message is the second defect: §5.1's own never-implicit bullet says
// "This bullet governs **non-constant** expressions; an integer compile-time
// constant (§17.14) converts by value under constant narrowing (below), which
// may cross signedness", so citing it here names a clause that explicitly does
// not govern. `primitiveNeverImplicitReason` already had the constant-narrowing
// bullet above it in its precedence order — it just could not be reached,
// because its own gate tested `from == P::Int`.
//
// THE FIX is one gate (`ExprTC::narrowIntConstantToTarget`, the project's ONE
// §5.1 constant-narrowing implementation) plus the diagnostic's twin gate, so
// an out-of-range WIDE constant reports the constant-narrowing bullet rather
// than the signedness one. The target side is expressed as "an integer target
// §5.1's ordinary widening table does not already admit from this source",
// which for an `int` source is exactly the six types the pre-fix list named —
// the rule is unchanged for the shapes it already admitted, and `int` becomes
// reachable as a target (the spec's second named example).
//
// RELAXATION, so the oracle direction is the risky one: every row below that
// expects ACCEPTANCE is unsatisfiable on 26ca3bd9, and every row that expects
// REJECTION pins a rejection the fix must NOT remove. The reject rows are the
// ones that make this a rule change rather than a blanket admission: §5.1's
// "never a silent truncation" sentence still governs, and an out-of-range
// wide constant must still be a located compile error.
//
// §22.6 overload-resolution APPLICABILITY is REACHED, not skipped. The
// function header's own comment says the rule "participates in
// overload-resolution applicability" is not served here because
// `implicitlyConvertible` takes TYPES only; MEASURED on this fix, §22.6's
// per-candidate grid builder calls `narrowIntConstantToTarget` directly with
// the AST argument expression (ExprTypecheck_Calls.cpp:1290, #2985), so the
// source-gate relaxation reaches applicability as well. The overload rows
// below pin that, with an `int`-literal CONTROL that #2985 already made pass.

#include <string>

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"          // runAot
#include "test_harness.hpp"

using namespace bezel::test;

namespace {

std::string prog(const std::string& body) {
    return "module M6554;\n"
           "public class C\n"
           "  public static method Main(): int\n" + body +
           "  end Main;\n"
           "end C;\n";
}

// `r.ran == false` alone is satisfied by a crash, a signal, a timeout and an
// ASan report as well as by a genuine rejection; the WORDING discriminates.
bool rejectedWith(const JitResult& r, const char* msg) {
    return failedAtCompileTimeWithRealLocation(r) &&
           diagnosticMessageContains(r, msg);
}

// §5.1's constant-narrowing bullet, in the spec's own words. A row expecting
// this must NOT be satisfied by the never-implicit signedness bullet.
const char* kDoesNotFit =
    "a constant whose value does not fit the target's range is a compile "
    "error at the conversion site";

// The bullet that names no constant exception and so must never be reported
// for one. Asserted as an ABSENCE on a rejection row: pre-fix, this program
// produced exactly this message, so pinning its absence is what makes the
// diagnostic half of the fix falsifiable.
const char* kSignedness =
    "between signed and unsigned types of the same or narrower width is "
    "never implicit";

// stdout of a program that must compile, run and exit 0. `ran == false` and a
// non-zero exit both read back as markers no expectation equals, so a
// rejection FAILS an accept row rather than passing it vacuously.
std::string out(const std::string& body) {
    auto r = runAot(prog(body));
    if (!r.ran) return "<did not run>";
    if (r.exitCode != 0)
        return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

}  // namespace

// ── The spec's FIRST verbatim example, at a plain local initializer ────────
//
//   const N: long := 8; var b: byte := N;
//
// The control row is the `int`-typed constant that always worked, so a row
// that passed merely because the harness compiled something cannot pass here.

BEZEL_TEST(wide_const_narrowing_6554,
           spec_verbatim_long_const_to_byte_spec_5_1) {
    BEZEL_EXPECT_EQ(
        out("    const N: long := 8;\n"
            "    var b: byte := N;\n"
            "    Console.WriteLine($\"{b}\");\n"
            "    return 0;\n"),
        "8\n");
}

BEZEL_TEST(wide_const_narrowing_6554,
           int_const_to_byte_control_spec_5_1) {
    // Byte-for-byte the same program with the source left at `int`. This is
    // the CONTROL ARM: it passes pre- and post-fix, so a green row 1 cannot be
    // explained by the harness accepting the shape for some other reason.
    BEZEL_EXPECT_EQ(
        out("    const N: int := 8;\n"
            "    var b: byte := N;\n"
            "    Console.WriteLine($\"{b}\");\n"
            "    return 0;\n"),
        "8\n");
}

// ── The spec's SECOND verbatim example: a `uint` constant where `int` is
//    expected — the pair the pre-fix target list could not even reach, since
//    `int` is not one of the six types an `int` source narrows to. ─────────

BEZEL_TEST(wide_const_narrowing_6554,
           spec_verbatim_uint_const_to_int_spec_5_1) {
    BEZEL_EXPECT_EQ(
        out("    const U: uint := 7;\n"
            "    var b: int := U;\n"
            "    Console.WriteLine($\"{b}\");\n"
            "    return 0;\n"),
        "7\n");
}

// ── The rule is about the VALUE, not the source's type: the same source
//    spelling admits or rejects purely on the folded value. These two rows
//    differ only in the literal. ───────────────────────────────────────────

BEZEL_TEST(wide_const_narrowing_6554,
           wide_const_in_range_admitted_spec_5_1) {
    BEZEL_EXPECT_EQ(
        out("    const N: long := 255;\n"
            "    var b: byte := N;\n"
            "    Console.WriteLine($\"{b}\");\n"
            "    return 0;\n"),
        "255\n");
}

BEZEL_TEST(wide_const_narrowing_6554,
           wide_const_out_of_range_still_rejected_spec_5_1) {
    // §5.1: "A constant whose value does not fit the target's range is a
    // compile error at the conversion site — never a silent truncation." The
    // source is a `long` and the value 256 does not fit `byte`.
    auto r = runAot(prog("    const N: long := 256;\n"
                         "    var b: byte := N;\n"
                         "    Console.WriteLine($\"{b}\");\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_TRUE(rejectedWith(r, kDoesNotFit));
    // The rejection must name the constant-narrowing bullet, not the
    // never-implicit signedness one. Pre-fix this program reported the
    // signedness bullet (its gate tested `from == P::Int`, so a `long`
    // source never reached the constant check above it).
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kSignedness));
}

BEZEL_TEST(wide_const_narrowing_6554,
           uint_const_out_of_int_range_still_rejected_spec_5_1) {
    // The range check that guards the NEW target: `int` was unreachable
    // before #6554, so `fitsInIntType`'s `default: true` arm had never been
    // asked about it. Without the explicit `Int` arm this row would COMPILE
    // and silently truncate 3000000000 to a negative `int`.
    auto r = runAot(prog("    const U: uint := 3000000000U;\n"
                         "    var b: int := U;\n"
                         "    Console.WriteLine($\"{b}\");\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_TRUE(rejectedWith(r, kDoesNotFit));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kSignedness));
}

BEZEL_TEST(wide_const_narrowing_6554,
           uint_const_over_int_max_rejected_at_int_spec_5_1) {
    // The same pair one step further out: 2147483648 is `uint`'s and not
    // `int`'s. Distinct from the row above (3000000000U) so a gate that
    // admitted one bound but not the other cannot pass both.
    auto r = runAot(prog("    const U: uint := 2147483648U;\n"
                         "    var b: int := U;\n"
                         "    Console.WriteLine($\"{b}\");\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_TRUE(rejectedWith(r, kDoesNotFit));
}

BEZEL_TEST(wide_const_narrowing_6554,
           ulong_const_to_uint_admitted_spec_5_1) {
    // A target the pre-fix list DID contain, from a source it did not admit.
    // Keeps the fix's positive side from being only about `int`.
    BEZEL_EXPECT_EQ(
        out("    const K: ulong := 42;\n"
            "    var v: uint := K;\n"
            "    Console.WriteLine($\"{v}\");\n"
            "    return 0;\n"),
        "42\n");
}

// ── "Constant narrowing applies at EVERY position where this section's
//    implicit conversions apply — there is no position carve-out". The
//    issue reported the rule failing at a local initializer; these rows show
//    it is not initializer-specific, because the one gate is called from
//    checkAssignable. ───────────────────────────────────────────────────────

BEZEL_TEST(wide_const_narrowing_6554,
           wide_const_at_call_argument_spec_5_1) {
    auto r = runAot(
        "module M6554arg;\n"
        "public class C\n"
        "  public static method Take(b: byte): int => int(b);\n"
        "  public static method Main(): int\n"
        "    const N: long := 200;\n"
        "    Console.WriteLine($\"{C.Take(N)}\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 0);
    BEZEL_EXPECT_EQ(r.ran ? r.stdoutText : "<did not run>", "200\n");
}

BEZEL_TEST(wide_const_narrowing_6554,
           wide_const_at_assignment_spec_5_1) {
    BEZEL_EXPECT_EQ(
        out("    const N: long := 77;\n"
            "    var b: byte := 0;\n"
            "    b := N;\n"
            "    Console.WriteLine($\"{b}\");\n"
            "    return 0;\n"),
        "77\n");
}

BEZEL_TEST(wide_const_narrowing_6554,
           wide_const_in_collection_literal_spec_5_1) {
    // §5.1 names "collection-literal elements (§15.4)" among the positions.
    BEZEL_EXPECT_EQ(
        out("    const N: long := 5;\n"
            "    var a: Array<byte> := [N, 6];\n"
            "    Console.WriteLine($\"{a[0]},{a[1]}\");\n"
            "    return 0;\n"),
        "5,6\n");
}

BEZEL_TEST(wide_const_narrowing_6554,
           wide_const_into_nullable_target_spec_5_1) {
    // §5.1 "Nullable conversions": "S converts to T? via S -> T -> T?", and
    // "constant narrowing (above) participates for a present constant value".
    BEZEL_EXPECT_EQ(
        out("    const N: long := 250;\n"
            "    var q: byte? := N;\n"
            "    Console.WriteLine($\"{q ?? -1}\");\n"
            "    return 0;\n"),
        "250\n");
}

// ── NEGATIVE control: the source must be a CONSTANT. §5.1's last clause of
//    the narrowing paragraph — "The rule applies to constants only; a run-time
//    `int` value still requires the explicit `byte(x)` narrowing of §5.2" —
//    must survive the source-gate relaxation unchanged. ─────────────────────

BEZEL_TEST(wide_const_narrowing_6554,
           non_constant_long_still_rejected_spec_5_1) {
    // A `long` VARIABLE is not a compile-time constant, so §5.1's table and
    // never-implicit list govern: `long -> byte` is neither. Pre-fix this
    // rejected for the same reason; the row exists so the relaxation cannot
    // be read as admitting wide-typed non-constants too.
    auto r = runAot(prog("    var n: long := 8;\n"
                         "    var b: byte := n;\n"
                         "    Console.WriteLine($\"{b}\");\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kDoesNotFit));
}

// ── §22.6 overload-resolution APPLICABILITY. The lane brief predicted this
//    path was UNREACHABLE from a source-gate fix — "`implicitlyConvertible`
//    takes TYPES only and cannot see the constant" — and that prediction is
//    REFUTED by measurement. §22.6's per-candidate grid builder calls
//    `narrowIntConstantToTarget` DIRECTLY, with the AST argument expression
//    (`ExprTypecheck_Calls.cpp:1290`, added by #2985 for the `int`-literal
//    case), before `classifyArg` grades the cell. So relaxing the source gate
//    reaches applicability too, and §5.1's own words — constant narrowing
//    "participates in overload-resolution applicability (§22.6)" — hold on
//    this path as well. These rows pin the widened case; the `int`-literal
//    row is the CONTROL that #2985 already made pass. ─────────────────────

BEZEL_TEST(wide_const_narrowing_6554,
           overload_wide_const_picks_byte_spec_22_6) {
    auto r = runAot(
        "module M6554ovl;\n"
        "public class C\n"
        "  public static method Pick(b: byte): int => 1;\n"
        "  public static method Pick(s: string): int => 2;\n"
        "  public static method Main(): int\n"
        "    const N: long := 3;\n"
        "    return C.Pick(N);\n"
        "  end Main;\n"
        "end C;\n");
    // Pre-fix: "no overload of `Pick` is applicable to these arguments; every
    // candidate would require a conversion that §5.1 does not admit as
    // implicit (§22.6)". Post-fix: `Pick(byte)` is the unique winner and the
    // program exits 1 — the value that distinguishes it from `Pick(string)`.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 1);
}

BEZEL_TEST(wide_const_narrowing_6554,
           overload_int_literal_picks_byte_control_spec_22_6) {
    // The CONTROL ARM: the same call with the source left at `int`, which
    // #2985 already admitted. Green both before and after this fix, so the
    // row above cannot be explained by the overload machinery generally
    // accepting a constant.
    auto r = runAot(
        "module M6554ovlc;\n"
        "public class C\n"
        "  public static method Pick(b: byte): int => 1;\n"
        "  public static method Pick(s: string): int => 2;\n"
        "  public static method Main(): int\n"
        "    return C.Pick(3);\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 1);
}

BEZEL_TEST(wide_const_narrowing_6554,
           overload_wide_const_out_of_range_still_rejected_spec_22_6) {
    // The rule still does not admit a constant the target cannot hold, and
    // at the overload path that means NO candidate is applicable — the same
    // verdict as no operator existing at all (§25), which is what
    // #5063's T2 already pins for an `int` literal.
    auto r = runAot(
        "module M6554ovlx;\n"
        "public class C\n"
        "  public static method Pick(b: byte): int => 1;\n"
        "  public static method Pick(s: string): int => 2;\n"
        "  public static method Main(): int\n"
        "    const N: long := 300;\n"
        "    return C.Pick(N);\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}