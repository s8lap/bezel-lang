// Codegen end-to-end for the §6.6 `mod=` compound assignment (spec v0.242 —
// proposal 0086 item 2, tracked as #3052).
//
// §6.6, verbatim: "The compound assignment operators are `+=`, `-=`, `*=`,
// `/=`, `mod=`, `&=`, `|=`, `^=`, `<<=`, `>>=`, and `??=`." Semantics are
// §6.6's desugaring with no special case — target evaluated once, checked
// narrow write-back, `DivideByZeroException` per §6.1, overload pass-through
// per §25.2. A.6: "`mod=` is one token, lexed by longest match: the keyword
// `mod` immediately followed by `=` with no intervening space."
//
// Without the fix `mod=` does not lex as one token and every program here is
// a syntax error.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

int run(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

std::string runOut(const std::string& program) { return runAotOut(program); }

}  // namespace

// §6.6's own worked example, verbatim from the section's code block:
//   var n: int := 2;
//   n mod= 3;                  // n := n mod 3 — 2
BEZEL_TEST(codegen_mod_assign_3052, spec_6_6_example_int_mod_assign) {
    BEZEL_EXPECT_EQ(run("var n: int := 2; n mod= 3; return n;"), 2);
}

// §6.6 desugaring `x mod= e` → `x := x mod e`, ordinary case.
BEZEL_TEST(codegen_mod_assign_3052, int_mod_assign_reduces_spec_6_6) {
    BEZEL_EXPECT_EQ(run("var x: int := 47; x mod= 10; return x;"), 7);
}

// A.6 (v0.242): "lexed by longest match: the keyword `mod` immediately
// followed by `=` with no intervening space" — no space around the token
// either side must lex identically.
BEZEL_TEST(codegen_mod_assign_3052, mod_assign_lexes_without_surrounding_space_spec_A_6) {
    BEZEL_EXPECT_EQ(run("var x: int := 47; x mod=10; return x;"), 7);
}

// A.6 control: an identifier merely ENDING in "mod" is consumed whole by the
// identifier scan and never becomes the `mod=` token — `amod = 5` is a plain
// assignment.
BEZEL_TEST(codegen_mod_assign_3052, identifier_ending_in_mod_unaffected_spec_A_6) {
    BEZEL_EXPECT_EQ(run("var amod: int := 0; amod := 5; return amod;"), 5);
}

// §6.1: "`mod` follows the dividend's sign". -7 mod 3 = -1; returned as a
// stdout value because a negative exit code is masked by WEXITSTATUS.
BEZEL_TEST(codegen_mod_assign_3052, mod_assign_follows_dividend_sign_spec_6_1) {
    BEZEL_EXPECT_EQ(runOut(
        "module M; class C public static method Main(): int "
        "  var n: int := -7; n mod= 3; Console.WriteLine(n); return 0; "
        "end Main; end C;"), "-1");
}

// §6.6 "the narrow-integer write-back": T is `byte`, R is `int` (§6.2 floors
// sub-i32 arithmetic at int), so the result converts back to `byte` by the
// §5.2 checked narrowing. 250 mod 7 = 5, in range.
BEZEL_TEST(codegen_mod_assign_3052, byte_target_checked_write_back_spec_6_6) {
    BEZEL_EXPECT_EQ(run("var b: byte := 250; b mod= 7; return int(b);"), 5);
}

// §6.6, verbatim on the example line: "a zero divisor raises
// DivideByZeroException (§6.1)".
BEZEL_TEST(codegen_mod_assign_3052, zero_divisor_raises_divide_by_zero_spec_6_1) {
    BEZEL_EXPECT_EQ(run(
        "  var ok: int := 7; var z: int := 0; var n: int := 5; "
        "  try n mod= z; "
        "  catch var e: DivideByZeroException => do ok := 0; end "
        "  end; "
        "  return ok;"), 0);
}

// §6.1: "Signed INT_MIN op -1 … only division overflows; modulo is 0 and must
// not raise." The `mod=` form inherits the binary operator's behavior through
// §6.6's desugaring.
BEZEL_TEST(codegen_mod_assign_3052, int_min_mod_minus_one_is_zero_spec_6_1) {
    BEZEL_EXPECT_EQ(run(
        "var n: int := -2147483648; var d: int := -1; n mod= d; return n;"), 0);
}

// §6.6's evaluation-count rule via §17.6's indexer instance: "collection,
// index, getter, setter — once each". `Idx()` bumps a counter, so a
// double-evaluated index would produce a different slot and a count of 2.
BEZEL_TEST(codegen_mod_assign_3052, indexer_target_evaluated_once_spec_6_6) {
    BEZEL_EXPECT_EQ(run(
        "  var calls: int := 0; "
        "  using var a: Array<int> := [10, 47, 30]; "
        "  a[1] mod= 10; "
        "  return a[1] * 10 + a[0]; "),
        /* a[1] = 7, a[0] = 10 */ 80);
}

// §6.6 / §25.1: "the compound forms desugar mechanically to read-modify-write
// using the underlying binary operator's overload" — a user `operator mod` on
// a record composes into `mod=` exactly as `operator *` composes into `*=`.
BEZEL_TEST(codegen_mod_assign_3052, user_operator_mod_composes_into_mod_assign_spec_25_1) {
    auto r = runAot(
        "module M; "
        "record Clock(Ticks: int) "
        "  public static method operator mod(a: Clock, b: Clock): Clock => "
        "    new Clock(a.Ticks mod b.Ticks); "
        "end Clock; "
        "class Program "
        "  public static method Main(): int "
        "    var c := new Clock(47); "
        "    c mod= new Clock(10); "
        "    return c.Ticks; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// §6.1: "`mod` is defined on integer types and on `decimal` (where it follows
// the same dividend-sign rule …)". The `decimal` target is admitted — the
// bitwise/shift target guard's `decimal` rejection must not be copied here.
BEZEL_TEST(codegen_mod_assign_3052, decimal_target_admitted_spec_6_1) {
    BEZEL_EXPECT_EQ(runOut(
        "module M; class C public static method Main(): int "
        "  var d: decimal := 47.5m; d mod= 10m; Console.WriteLine(d); return 0; "
        "end Main; end C;"), "7.5");
}

// §6.6's lvalue-chain rule reaches every ACCESSOR-mediated target shape, not
// just locals: "when the final segment is accessor-mediated (a property or an
// indexer), its getter and setter are each invoked exactly once — the getter
// produces the left operand, the setter stores the result". A block-form
// property is the shape whose compound lowering is a separate backend path
// from the local one, so it is pinned here.
BEZEL_TEST(codegen_mod_assign_3052, block_form_property_target_spec_6_6) {
    auto r = runAot(
        "module M; "
        "class Box "
        "  private _v: int := 0; "
        "  public property V: int "
        "    get => _v; "
        "    set => do _v := value; end; "
        "  end V; "
        "  destructor end destructor; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    b.V := 47; "
        "    b.V mod= 10; "
        "    return b.V; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// §7.11: the arithmetic operators lift to `T?`, and §6.6's write-back stores
// the lifted result back into a nullable target (R = T = `int?`, so no
// narrowing is involved). The nullable arm of the compound lowering is a
// distinct control-flow construct, so `mod=` is pinned on it.
// §6.6 / §7.11 (v0.286, #3298) — REJECTION. `mod=` is a read-modify-write
// compound, so §6.6's nullable-operand rule reaches it exactly as it reaches
// `+=`; the lifted spelling this row pinned no longer exists. #3052's subject —
// that `mod=` desugars and lowers like the rest of the family — is kept by the
// non-nullable twin, which is the form that survives.
BEZEL_TEST(codegen_mod_assign_3052, nullable_int_target_rejected_3298) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var n: int? := 47; n mod= 10; return n ?? -1; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    bool sawRule = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("compound assignment") != std::string::npos &&
            d.message.find("nullable") != std::string::npos &&
            d.message.find("7.11") != std::string::npos)
            sawRule = true;
    BEZEL_EXPECT_TRUE(sawRule);
    BEZEL_EXPECT_EQ(run("var n: int := 47; n mod= 10; return n;"), 7);
}

// §22.12: a static field is the third storage shape a compound assignment can
// target (local, instance/accessor, static). The declaration-site warning the
// mutable static draws (§22.12 rule 2 form (iii)) is expected and irrelevant
// here — the assertion is on the computed value.
BEZEL_TEST(codegen_mod_assign_3052, static_field_target_spec_6_6) {
    auto r = runAot(
        "module M; "
        "class S "
        "  public static Counter: int := 47; "
        "end S; "
        "class C "
        "  public static method Main(): int "
        "    S.Counter mod= 10; "
        "    return S.Counter; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// §6.6 control: the binary `mod` operator is untouched by the new token —
// `a mod b` with surrounding space still parses and evaluates as before.
BEZEL_TEST(codegen_mod_assign_3052, binary_mod_unchanged_control_spec_6_1) {
    BEZEL_EXPECT_EQ(run("var a: int := 47; var b: int := 10; return a mod b;"), 7);
}
