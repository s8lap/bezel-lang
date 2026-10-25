// Codegen + runtime — Math constants/rounding and Convert.TryToInt32, filling
// gaps against the standard library spec:
//
//   stdlib §2.2 Math:
//     public static const Pi:  double := 3.14159265358979323846;
//     public static const Tau: double := 6.28318530717958647692;
//     public static method Round(x: double): double;            // banker's rounding
//     public static method Round(x: double, digits: int): double;
//
//   stdlib §2.3 Convert:
//     public static method TryToInt32(value: string, out result: int): bool;
//
// Before this milestone the compiler implemented `Math.PI` (the wrong spelling —
// the spec constant is `Pi`, §2.2 line "public static const Pi") and had no
// `Tau`, no two-argument `Round`, and round-half-AWAY-from-zero for one-argument
// `Round` (the spec mandates banker's rounding, i.e. round-half-to-EVEN). It
// also lacked `Convert.TryToInt32`. These tests pin the spec-stated behavior.
//
// Three layers (this milestone):
//   - Codegen: Math.Pi / Math.Tau constants and Round(x)/Round(x,digits) banker's
//     rounding in LlvmBackend_Calls.cpp + the constant member-access path in
//     LlvmBackend_Exprs.cpp; Convert.TryToInt32 out-param marshalling in
//     LlvmBackend_Calls.cpp.
//   - Sema: typing of Math.Pi/Math.Tau (double) and Convert.TryToInt32 (bool)
//     in ExprTypecheck_Calls.cpp.
//   - Runtime: none needed (all map to LLVM intrinsics / libc strtol).
//
// Value tests run through the JIT harness (exit code = Main()'s return). Since
// the JIT reads the raw int return (not the 8-bit-masked process exit status),
// tests can return values above 255 directly.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;
using bezel::test::runAotChecked;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

bool hasSemaError(const std::string& src) {
    auto r = runAot(src);
    return !r.ran && r.diagnostics.hasErrors();
}

std::string mainBody(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           " end Main; end C;";
}

}  // namespace

// ===========================================================================
// #1037 — Math and Convert are distinct classes; cross-class and unknown
// statics must be REJECTED (§3.4), not silently miscompiled.
//
// The sema accept logic used to type the UNION of both classes' methods on
// either class (one `cls == "Convert" || cls == "Math"` block), and the
// builtin-class path (null decl) had no fallback "no such member" reject. So
// `Math.ToInt32(5)` (→45), `Convert.Sqrt(16.0)` (→0) and even `Math.Bogus(5)`
// (→garbage) compiled and miscompiled. They must now be compile errors.
// ===========================================================================

// Convert-only method on Math → reject.
BEZEL_TEST(math_convert_1037, math_toint32_rejected) {
    BEZEL_EXPECT_TRUE(hasSemaError(mainBody("var x: int := Math.ToInt32(5); return x;")));
}

// Math-only method on Convert → reject.
BEZEL_TEST(math_convert_1037, convert_sqrt_rejected) {
    BEZEL_EXPECT_TRUE(hasSemaError(mainBody("var x: double := Convert.Sqrt(16.0); return int(x);")));
}

// Fully-unknown static on a builtin class → reject (was an unrejected hole).
BEZEL_TEST(math_convert_1037, math_unknown_rejected) {
    BEZEL_EXPECT_TRUE(hasSemaError(mainBody("var x: int := Math.Bogus(5); return x;")));
}
BEZEL_TEST(math_convert_1037, convert_unknown_rejected) {
    BEZEL_EXPECT_TRUE(hasSemaError(mainBody("var x: int := Convert.Bogus(5); return x;")));
}

// Positive controls — real methods on the right class must STILL work.
BEZEL_TEST(math_convert_1037, math_sqrt_still_works) {
    BEZEL_EXPECT_EQ(run(mainBody("var x: double := Math.Sqrt(16.0); return int(x);")), 4);
}
BEZEL_TEST(math_convert_1037, math_min_still_works) {
    BEZEL_EXPECT_EQ(run(mainBody("return Math.Min(3, 8);")), 3);
}
BEZEL_TEST(math_convert_1037, math_clamp_still_works) {
    BEZEL_EXPECT_EQ(run(mainBody("return Math.Clamp(5, 1, 4);")), 4);
}
BEZEL_TEST(math_convert_1037, convert_toint32_still_works) {
    // stdlib §2.3 declares `ToInt32(value: string): int` — the `7` spelling
    // this row used is REJECTED since #7218 (§5.1: no implicit conversion
    // reaches a `string` parameter, and §22.6 admits a call that matches no
    // declared form only when the argument converts to one), so the row
    // reads the DECLARED form. The row's claim — that `Convert`'s statics
    // are `Convert`'s own and still work (#1037) — is unchanged.
    BEZEL_EXPECT_EQ(run(mainBody("var x: int := Convert.ToInt32(\"7\"); return x;")), 7);
}

// --- Math.Pi (stdlib §2.2, "public static const Pi: double := 3.14159...") ---
// The spec spelling is `Pi`. Returns 1 if Pi is within (3.14, 3.15).
BEZEL_TEST(math_convert, math_pi_constant_spec_2_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var p := Math.Pi; "
        "  return (if p > 3.14 and p < 3.15 then 1 else 0); "
        "end Main; end C;"), 1);
}

// --- Math.Tau (stdlib §2.2, "public static const Tau: double := 6.28318...") ---
// Tau = 2*Pi. Returns 1 if Tau is within (6.28, 6.29) and ~2*Pi.
BEZEL_TEST(math_convert, math_tau_constant_spec_2_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var t := Math.Tau; "
        "  var d := t - 2.0 * Math.Pi; "
        "  return (if t > 6.28 and t < 6.29 and Math.Abs(d) < 0.0001 then 1 else 0); "
        "end Main; end C;"), 1);
}

// --- Math.Round(x): banker's rounding (round half to even) — stdlib §2.2 ---
// Round(2.5) = 2, Round(3.5) = 4, Round(0.5) = 0, Round(1.5) = 2.
// Encodes the four results as a single integer: r25*1000 + r35*100 + r05*10 + r15.
// Expected: 2*1000 + 4*100 + 0*10 + 2 = 2402.
BEZEL_TEST(math_convert, math_round_bankers_spec_2_2) {
    // 2402 does not fit an 8-bit exit code — observe on stdout.
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "  var r25 := int(Math.Round(2.5)); "
        "  var r35 := int(Math.Round(3.5)); "
        "  var r05 := int(Math.Round(0.5)); "
        "  var r15 := int(Math.Round(1.5)); "
        "  Console.WriteLine(r25 * 1000 + r35 * 100 + r05 * 10 + r15); return 0; "
        "end Main; end C;"), "2402");
}

// --- Math.Round(x): ordinary (non-half) cases round to nearest — stdlib §2.2 ---
// Round(2.4) = 2, Round(2.6) = 3, Round(-2.6) = -3.
// Encode: (r24 + 10) * 100 + (r26 + 10) * 10 + (r_neg + 10)
//       = 12*100 + 13*10 + 7 = 1337.
BEZEL_TEST(math_convert, math_round_nearest_spec_2_2) {
    // 1337 does not fit an 8-bit exit code — observe on stdout.
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "  var r24 := int(Math.Round(2.4)); "
        "  var r26 := int(Math.Round(2.6)); "
        "  var rn  := int(Math.Round(0.0 - 2.6)); "
        "  Console.WriteLine((r24 + 10) * 100 + (r26 + 10) * 10 + (rn + 10)); return 0; "
        "end Main; end C;"), "1337");
}

// --- Math.Round(x, digits): round to N fractional digits — stdlib §2.2 ---
// Round(3.14159, 2) = 3.14, Round(3.14159, 4) = 3.1416.
// Returns 1 if Round(3.14159, 2) is within (3.139, 3.141) and
//                Round(3.14159, 4) is within (3.1415, 3.1417).
BEZEL_TEST(math_convert, math_round_digits_spec_2_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := Math.Round(3.14159, 2); "
        "  var b := Math.Round(3.14159, 4); "
        "  return (if a > 3.139 and a < 3.141 and b > 3.1415 and b < 3.1417 then 1 else 0); "
        "end Main; end C;"), 1);
}

// --- Math.Round(x, digits): banker's rounding at the digit boundary — §2.2 ---
// Round(2.125, 2): 2.125 * 100 = 212.5, round-half-to-even -> 212, / 100 = 2.12.
// Assert the result lands in (2.119, 2.121).
BEZEL_TEST(math_convert, math_round_digits_bankers_spec_2_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := Math.Round(2.125, 2); "
        "  return (if a > 2.119 and a < 2.121 then 1 else 0); "
        "end Main; end C;"), 1);
}

// --- Convert.TryToInt32: success — stdlib §2.3 ---
// TryToInt32("42", out n) returns true and sets n = 42.
// Return 1 if ok and n == 42, else 0.
BEZEL_TEST(math_convert, convert_tryto_int32_ok_spec_2_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var n: int := 0; "
        "  var ok := Convert.TryToInt32(\"42\", out n); "
        "  return (if ok and n = 42 then 1 else 0); "
        "end Main; end C;"), 1);
}

// --- Convert.TryToInt32: failure — stdlib §2.3 ---
// TryToInt32("abc", out n) returns false (no exception). Return 1 if not ok.
BEZEL_TEST(math_convert, convert_tryto_int32_fail_spec_2_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var n: int := 0; "
        "  var ok := Convert.TryToInt32(\"abc\", out n); "
        "  return (if not ok then 1 else 0); "
        "end Main; end C;"), 1);
}

// --- Convert.TryToInt32: trailing junk is a failure — stdlib §2.3 ---
// "12x" is not a fully-valid integer, so TryToInt32 returns false.
BEZEL_TEST(math_convert, convert_tryto_int32_trailing_junk_spec_2_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var n: int := 0; "
        "  var ok := Convert.TryToInt32(\"12x\", out n); "
        "  return (if not ok then 1 else 0); "
        "end Main; end C;"), 1);
}

// ── #1141 / stdlib §1 / §14.2: Math.Min/Max/Clamp over a NON-NUMERIC ──────────
// IComparable<T> value type. Math.Min/Max/Clamp are declared ONLY as the
// generic `Min<T>(a, b): T where T: IComparable<T>` (no numeric overload), so a
// record/enum that implements IComparable<T> is a valid type argument (§14.2).
// Before the fix the intrinsic was hard-wired to a numeric sitofp-to-double
// lowering, so a record operand emitted `sitofp %record … to double` →
// malformed IR / compile abort; sema also left the result mistyped (defaulting
// to int), so a `.field` access on the result folded to 0. Codegen now routes
// the comparison through the type's `CompareTo`, and sema types the result as T.
namespace {
// A value type (record) implementing IComparable<Money>.
const char* kMoney =
    "module M; import System; "
    "record Money(cents: int): IComparable<Money> "
    "  public method CompareTo(other: Money): int => "
    "    (if cents > other.cents then 1 else (if cents < other.cents then -1 else 0)); "
    "end Money; ";
std::string moneyMain(const std::string& body) {
    return std::string(kMoney) +
           "class App public static method Main(): int " + body +
           " end Main; end App;";
}
}  // namespace

// #1141: Math.Min<Money> returns the lesser operand (CompareTo dispatch), not
// an IR_FAIL — Money(150) vs Money(99) → 99.
BEZEL_TEST(math_comparable_1141, min_record_explicit_type_arg) {
    BEZEL_EXPECT_EQ(run(moneyMain(
        "  var a := new Money(150); var b := new Money(99); "
        "  var lo := Math.Min<Money>(a, b); "
        "  return lo.cents;")), 99);
}

// #1141: the INFERRED form `Math.Min(a, b)` also dispatches via CompareTo.
BEZEL_TEST(math_comparable_1141, min_record_inferred) {
    BEZEL_EXPECT_EQ(run(moneyMain(
        "  var a := new Money(150); var b := new Money(99); "
        "  return Math.Min(a, b).cents;")), 99);
}

// #1141: Math.Max<Money> returns the greater operand → 150.
BEZEL_TEST(math_comparable_1141, max_record) {
    BEZEL_EXPECT_EQ(run(moneyMain(
        "  var a := new Money(150); var b := new Money(99); "
        "  return Math.Max<Money>(a, b).cents;")), 150);
}

// #1141: Math.Clamp<Money> — below range clamps to lo (80 → 99).
BEZEL_TEST(math_comparable_1141, clamp_record_below_range) {
    BEZEL_EXPECT_EQ(run(moneyMain(
        "  var v := new Money(80); var lo := new Money(99); var hi := new Money(150); "
        "  return Math.Clamp<Money>(v, lo, hi).cents;")), 99);
}

// #1141: Math.Clamp<Money> — above range clamps to hi (200 → 150).
BEZEL_TEST(math_comparable_1141, clamp_record_above_range) {
    BEZEL_EXPECT_EQ(run(moneyMain(
        "  var v := new Money(200); var lo := new Money(99); var hi := new Money(150); "
        "  return Math.Clamp<Money>(v, lo, hi).cents;")), 150);
}

// #1141: Math.Clamp<Money> — in range returns the value unchanged (120 → 120).
BEZEL_TEST(math_comparable_1141, clamp_record_in_range) {
    BEZEL_EXPECT_EQ(run(moneyMain(
        "  var v := new Money(120); var lo := new Money(99); var hi := new Money(150); "
        "  return Math.Clamp<Money>(v, lo, hi).cents;")), 120);
}

// #1141 CONTROL: the numeric int path is unaffected — Min(8,3)=3, Max(8,3)=8,
// Clamp(20,0,10)=10 → 3*100 + 8 + 10 = 318. 318 does not fit an 8-bit exit
// code — observe on stdout.
BEZEL_TEST(math_comparable_1141, numeric_int_unaffected) {
    BEZEL_EXPECT_EQ(runAotOut(mainBody(
        "Console.WriteLine(Math.Min(8, 3) * 100 + Math.Max(8, 3) + Math.Clamp(20, 0, 10)); return 0;")),
        "318");
}

// #1141 CONTROL: the numeric double path is unaffected — Min(2.5,7.5)=2,
// Max=7, Clamp(99,0,10)=10 → 2*100 + 7 + 10 = 217.
BEZEL_TEST(math_comparable_1141, numeric_double_unaffected) {
    BEZEL_EXPECT_EQ(run(mainBody(
        "  var lo := Math.Min(2.5, 7.5); var hi := Math.Max(2.5, 7.5); "
        "  var c := Math.Clamp(99.0, 0.0, 10.0); "
        "  return int(lo) * 100 + int(hi) + int(c);")), 217);
}

// ── #1148 / stdlib §2.2 / §14.2: Math.Min/Max/Clamp<T> over a CLASS ──────────
// implementing IComparable<T>.  Before the fix, a class operand was sent to the
// numeric sitofp-to-double path — `sitofp ptr %15 to double` → malformed IR.
// The comparableCompareTo lambda now accepts Class declarations in addition to
// Record/Enum and dispatches CompareTo through emitMethodCall (vtable if the
// method is virtual/override, direct compiled call otherwise). The sema
// result-type gate likewise accepts Class so that member access on the borrow
// result resolves to the class's fields.
namespace {
// A heap class implementing IComparable<Box> (§14.2 / §11 class declarations).
const char* kBox =
    "module M; import System; "
    "class Box(v: int): IComparable<Box> "
    "  public readonly property V: int := v; "
    "  public method CompareTo(other: Box): int => "
    "    (if v > other.v then 1 else (if v < other.v then -1 else 0)); "
    "end Box; ";
std::string boxMain(const std::string& body) {
    return std::string(kBox) +
           "class App public static method Main(): int " + body +
           " end Main; end App;";
}
}  // namespace

// #1148: Math.Min<Box> returns the lesser class instance — Box(99) < Box(150).
BEZEL_TEST(math_comparable_1148, min_class_explicit_type_arg) {
    BEZEL_EXPECT_EQ(run(boxMain(
        "  using var a := new Box(150); using var b := new Box(99); "
        "  var lo := Math.Min<Box>(a, b); "
        "  return lo.V;")), 99);
}

// #1148: inferred form Math.Min(a, b) also dispatches via CompareTo.
BEZEL_TEST(math_comparable_1148, min_class_inferred) {
    BEZEL_EXPECT_EQ(run(boxMain(
        "  using var a := new Box(150); using var b := new Box(99); "
        "  return Math.Min(a, b).V;")), 99);
}

// #1148: Math.Max<Box> returns the greater class instance — Box(150) > Box(99).
BEZEL_TEST(math_comparable_1148, max_class) {
    BEZEL_EXPECT_EQ(run(boxMain(
        "  using var a := new Box(150); using var b := new Box(99); "
        "  return Math.Max<Box>(a, b).V;")), 150);
}

// #1148: Math.Clamp<Box> — below range clamps to lo (Box(80) → Box(99)).
BEZEL_TEST(math_comparable_1148, clamp_class_below_range) {
    BEZEL_EXPECT_EQ(run(boxMain(
        "  using var v := new Box(80); "
        "  using var lo := new Box(99); using var hi := new Box(150); "
        "  return Math.Clamp<Box>(v, lo, hi).V;")), 99);
}

// #1148: Math.Clamp<Box> — above range clamps to hi (Box(200) → Box(150)).
BEZEL_TEST(math_comparable_1148, clamp_class_above_range) {
    BEZEL_EXPECT_EQ(run(boxMain(
        "  using var v := new Box(200); "
        "  using var lo := new Box(99); using var hi := new Box(150); "
        "  return Math.Clamp<Box>(v, lo, hi).V;")), 150);
}

// #1148: Math.Clamp<Box> — in range returns the value unchanged (Box(120)).
BEZEL_TEST(math_comparable_1148, clamp_class_in_range) {
    BEZEL_EXPECT_EQ(run(boxMain(
        "  using var v := new Box(120); "
        "  using var lo := new Box(99); using var hi := new Box(150); "
        "  return Math.Clamp<Box>(v, lo, hi).V;")), 120);
}

// ── #1215 / §14.2: Math.Min/Max/Clamp over a NULLABLE class IComparable ──────
// §14.2 states that a type-bound constraint (e.g. `where T: IComparable<T>`)
// admits only NON-NULLABLE types: "nullable types do not themselves implement
// the interfaces of their non-nullable counterparts: a null value satisfies no
// interface contract." So `Math.Min(a, b)` where a,b: Box? must be rejected at
// sema with a §14.2 diagnostic — it must not silently reach codegen and produce
// a null-pointer CompareTo dispatch. The three operations Min/Max/Clamp all go
// through the same comparableCompareTo path so all three are tested.

// #1215: Math.Min with nullable class args → sema error (§14.2).
BEZEL_TEST(math_comparable_1215, min_nullable_class_rejected) {
    BEZEL_EXPECT_TRUE(hasSemaError(
        std::string(kBox) +
        "class App public static method Main(): int "
        "  using var a: Box? := new Box(150); using var b: Box? := new Box(99); "
        "  return Math.Min(a, b).V;"
        " end Main; end App;"));
}

// #1215: Math.Max with nullable class args → sema error (§14.2).
BEZEL_TEST(math_comparable_1215, max_nullable_class_rejected) {
    BEZEL_EXPECT_TRUE(hasSemaError(
        std::string(kBox) +
        "class App public static method Main(): int "
        "  using var a: Box? := new Box(150); using var b: Box? := new Box(99); "
        "  return Math.Max(a, b).V;"
        " end Main; end App;"));
}

// #1215: Math.Clamp with nullable class args → sema error (§14.2).
BEZEL_TEST(math_comparable_1215, clamp_nullable_class_rejected) {
    BEZEL_EXPECT_TRUE(hasSemaError(
        std::string(kBox) +
        "class App public static method Main(): int "
        "  using var v: Box? := new Box(120); "
        "  using var lo: Box? := new Box(99); using var hi: Box? := new Box(150); "
        "  return Math.Clamp(v, lo, hi).V;"
        " end Main; end App;"));
}

// #1215: explicit type arg Box? (nullable) also rejected (§14.2).
BEZEL_TEST(math_comparable_1215, min_nullable_explicit_type_arg_rejected) {
    BEZEL_EXPECT_TRUE(hasSemaError(
        std::string(kBox) +
        "class App public static method Main(): int "
        "  using var a: Box? := new Box(150); using var b: Box? := new Box(99); "
        "  return Math.Min<Box?>(a, b).V;"
        " end Main; end App;"));
}

// #1215: non-nullable class args are still accepted — regression control.
BEZEL_TEST(math_comparable_1215, min_nonnull_class_still_works) {
    BEZEL_EXPECT_EQ(run(boxMain(
        "  using var a := new Box(150); using var b := new Box(99); "
        "  return Math.Min(a, b).V;")), 99);
}

// ── #1875 / §14.2 (clause 4) / stdlib §2.2: enforce `where T: IComparable<T>` ─
// on Math.Min/Max/Clamp for a class/record/enum type argument. These selectors
// resolve to no MethodSymbol (handled as a builtin), so the ordinary
// generic-method constraint check never ran: a NON-conforming type (a class
// with no `: IComparable`, no CompareTo) was silently accepted at sema and then
// flowed to the numeric codegen path, which emitted malformed IR
// (`sitofp ptr/record to double` — no CompareTo to route through) and aborted
// codegen (IR verification failed). Sema now rejects a non-conforming type with
// a §14.2 constraint diagnostic, so the malformed-IR crash is unreachable.
//
// The `diagContains` probe pins the SPECIFIC fix: before the fix the compile
// still failed, but via the codegen IR-verification error (no "does not satisfy
// the constraint" text — for the enum case it did not fail at all); after the
// fix it fails at sema with the constraint diagnostic. So each test both
// fails-without and passes-with.
namespace {
// True iff at least one reported error message contains `needle`.
bool diagContains(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}
// A class that does NOT implement IComparable<T> (the issue-repro shape:
// no `: IComparable`, a `Cmp` method rather than `CompareTo`).
const char* kUncomparable =
    "module M; import System; "
    "class Plain(v: int) "
    "  public readonly property V: int := v; "
    "  public method Cmp(o: Plain): int return 0; end Cmp; "
    "end Plain; ";
std::string plainMain(const std::string& body) {
    return std::string(kUncomparable) +
           "class App public static method Main(): int " + body +
           " end Main; end App;";
}
const char* kConstraintNeedle = "does not satisfy the constraint";
}  // namespace

// #1875: Math.Max over a class NOT implementing IComparable<T> → §14.2 reject
// (previously: `sitofp ptr to double` malformed-IR codegen crash).
BEZEL_TEST(math_comparable_1875, max_nonconforming_class_rejected) {
    BEZEL_EXPECT_TRUE(diagContains(plainMain(
        "  using var a := new Plain(1); using var b := new Plain(2); "
        "  var m := Math.Max(a, b); return m.V;"),
        kConstraintNeedle));
}

// #1875: Math.Min over a non-conforming class → §14.2 reject.
BEZEL_TEST(math_comparable_1875, min_nonconforming_class_rejected) {
    BEZEL_EXPECT_TRUE(diagContains(plainMain(
        "  using var a := new Plain(1); using var b := new Plain(2); "
        "  var m := Math.Min(a, b); return m.V;"),
        kConstraintNeedle));
}

// #1875: Math.Clamp over a non-conforming class → §14.2 reject.
BEZEL_TEST(math_comparable_1875, clamp_nonconforming_class_rejected) {
    BEZEL_EXPECT_TRUE(diagContains(plainMain(
        "  using var v := new Plain(1); "
        "  using var lo := new Plain(0); using var hi := new Plain(2); "
        "  var m := Math.Clamp(v, lo, hi); return m.V;"),
        kConstraintNeedle));
}

// #1875: explicit type arg `Math.Max<Plain>` is rejected the same way (the
// check keys on the operand type, so inferred and explicit forms both reject).
BEZEL_TEST(math_comparable_1875, max_nonconforming_class_explicit_rejected) {
    BEZEL_EXPECT_TRUE(diagContains(plainMain(
        "  using var a := new Plain(1); using var b := new Plain(2); "
        "  var m := Math.Max<Plain>(a, b); return m.V;"),
        kConstraintNeedle));
}

// #1875: the same defect for a RECORD not implementing IComparable<T> — it also
// hit the sitofp-on-a-struct malformed-IR path. Now rejected at sema (§14.2).
BEZEL_TEST(math_comparable_1875, max_nonconforming_record_rejected) {
    BEZEL_EXPECT_TRUE(diagContains(
        "module M; import System; record Bare(v: int); "
        "class App public static method Main(): int "
        "  var a := new Bare(1); var b := new Bare(2); "
        "  var m := Math.Max(a, b); return m.v;"
        " end Main; end App;",
        kConstraintNeedle));
}

// #1875: an ENUM not listing IComparable<E> in its inheritance clause does NOT
// satisfy `where T: IComparable<T>` (§14.2 clause 4 — no implicit conformance).
// This case did not crash (enum ordinals lower to a valid sitofp), but it was
// silently accepted, a soundness hole. Now rejected at sema.
BEZEL_TEST(math_comparable_1875, max_nonconforming_enum_rejected) {
    BEZEL_EXPECT_TRUE(diagContains(
        "module M; import System; enum Color Red, Green, Blue; end Color; "
        "class App public static method Main(): int "
        "  var a := Color.Red; var b := Color.Blue; "
        "  var m := Math.Max(a, b); return 0;"
        " end Main; end App;",
        kConstraintNeedle));
}

// #1875 regression control: a class that DOES implement IComparable<Box> still
// compiles and runs — the fix must not over-reject a conforming type. Max
// returns the greater instance (Box(150) > Box(99)) via CompareTo dispatch.
BEZEL_TEST(math_comparable_1875, conforming_class_still_runs) {
    BEZEL_EXPECT_EQ(run(boxMain(
        "  using var a := new Box(150); using var b := new Box(99); "
        "  return Math.Max(a, b).V;")), 150);
}

// ── #1858 / §5 (Math.Min/Max/Clamp) / §11 (IComparable) / §14.2 ──────────────
// Math.Min/Max/Clamp over a GENERIC-record IComparable. The #1141 path handled
// only a NON-generic record; a generic instantiation (`Box<string>`) reached
// emitMethodCall with `genericSubst_` EMPTY at this intrinsic call site, so it
// fell through to a DIRECT call of the UNSPECIALIZED template `bz$Box$CompareTo`
// (param `%record.Box` = {i32,i32}) while passing the SPECIALIZED
// `%record.Box$spec0` receiver/argument — a "Call parameter type does not match
// function signature" IR-VERIFICATION FAILURE (the valid program was rejected at
// compile). The fix installs the per-instantiation generic substitution from
// the receiver's concrete type arguments BEFORE the call (so emitMethodCall
// routes through emitInlineGenericMethod and monomorphizes CompareTo), AND sizes
// the receiver spill slot to the monomorphized layout (`recv->getType()`) rather
// than the 4-byte i32-placeholder — the latent stack-buffer-overflow sibling of
// #1855 (an 8-byte specialized `string` pointer stored into a 4-byte slot).
//
// runAotChecked compiles + links + RUNS under generated-code AddressSanitizer:
// WITHOUT the fix `r.ran` is false (the module fails IR verification, so the
// program never links/runs); WITH it the program is ASan-clean AND value-
// correct. So each test both fails-without and passes-with.
namespace {
// A generic record with a REFERENCE-typed field (`value: T`) plus an int `key`;
// CompareTo orders by key (§11, IComparable<Box<T>>). This is the issue repro.
const char* kGenBox =
    "module M; import System; "
    "record Box<T>(value: T, key: int): IComparable<Box<T>> "
    "  public method CompareTo(other: Box<T>): int => this.key - other.key; "
    "end Box; ";
std::string genBoxMain(const std::string& body) {
    return std::string(kGenBox) +
           "class App public static method Main(): int " + body +
           " end Main; end App;";
}
}  // namespace

// #1858: Math.Min<Box<string>> (inferred) returns the lesser-by-key operand
// (key 1 < 2 → `a` = "aaa"), so .value.Length = 3. ASan-clean + value-correct.
BEZEL_TEST(math_comparable_1858, min_generic_record_string_inferred) {
    auto r = runAotChecked(genBoxMain(
        "  var a := new Box<string>(\"aaa\", 1); "
        "  var b := new Box<string>(\"bb\", 2); "
        "  return Math.Min(a, b).value.Length;"));
    BEZEL_EXPECT_TRUE(r.ran);         // WITHOUT fix: IR verification rejects → false
    BEZEL_EXPECT_EQ(r.exitCode, 3);   // "aaa".Length
}

// #1858: Math.Max<Box<string>> returns the greater-by-key operand (key 2 → `b`
// = "bb"), so .value.Length = 2.
BEZEL_TEST(math_comparable_1858, max_generic_record_string) {
    auto r = runAotChecked(genBoxMain(
        "  var a := new Box<string>(\"aaa\", 1); "
        "  var b := new Box<string>(\"bb\", 2); "
        "  return Math.Max(a, b).value.Length;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);   // "bb".Length
}

// #1858: the EXPLICIT nested-generic type argument `Math.Max<Box<string>>` is
// monomorphized the same way (the substitution keys on the operand type).
BEZEL_TEST(math_comparable_1858, max_generic_record_string_explicit_type_arg) {
    auto r = runAotChecked(genBoxMain(
        "  var a := new Box<string>(\"aaa\", 1); "
        "  var b := new Box<string>(\"bb\", 2); "
        "  return Math.Max<Box<string>>(a, b).value.Length;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);
}

// #1858: Math.Clamp<Box<string>> — value key 5 is ABOVE hi key 3, so it clamps
// to hi ("hhh"), .value.Length = 3. Exercises the two-CompareTo Clamp path.
BEZEL_TEST(math_comparable_1858, clamp_generic_record_string_above_range) {
    auto r = runAotChecked(genBoxMain(
        "  var v  := new Box<string>(\"vvvv\", 5); "
        "  var lo := new Box<string>(\"l\", 2); "
        "  var hi := new Box<string>(\"hhh\", 3); "
        "  return Math.Clamp(v, lo, hi).value.Length;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);   // clamped to hi = "hhh"
}

// #1858: a VALUE-typed generic field (`Box<int>`) monomorphizes correctly too —
// Min by key (3 vs 1 → `b`, key 1), then read `b.value` = 200.
BEZEL_TEST(math_comparable_1858, min_generic_record_int_value_field) {
    auto r = runAotChecked(genBoxMain(
        "  var a := new Box<int>(100, 3); "
        "  var b := new Box<int>(200, 1); "
        "  return Math.Min(a, b).value;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 200);
}

// #1858: a TWO-type-parameter generic record with a mixed value+reference field
// pair (`Pair<int, string>`) — Max by key (9 vs 4 → `x`), then a=7 + "hello"
// Length 5 = 12. Confirms the substitution binds all type params positionally.
BEZEL_TEST(math_comparable_1858, max_generic_record_two_type_params) {
    auto r = runAotChecked(
        "module M; import System; "
        "record Pair<A, B>(a: A, b: B, key: int): IComparable<Pair<A, B>> "
        "  public method CompareTo(other: Pair<A, B>): int => "
        "    this.key - other.key; "
        "end Pair; "
        "class App public static method Main(): int "
        "  var x := new Pair<int, string>(7, \"hello\", 9); "
        "  var y := new Pair<int, string>(3, \"hi\", 4); "
        "  return Math.Max(x, y).a + Math.Max(x, y).b.Length;"
        " end Main; end App;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 12);  // 7 + "hello".Length(5)
}

// #1858 regression control: the NON-generic record path (#1141) is unchanged —
// the substitution install is a no-op when the record has no type parameters.
BEZEL_TEST(math_comparable_1858, non_generic_record_unaffected) {
    auto r = runAotChecked(moneyMain(
        "  var a := new Money(150); var b := new Money(99); "
        "  return Math.Min(a, b).cents;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 99);
}

// ===========================================================================
// #1238 — Convert.To*/TryTo* strict validation (stdlib §2.3)
//
// Before this fix, Convert.ToInt32/ToInt64/ToDouble used atoi/atoll/atof which
// return 0 silently on invalid input — no exception was thrown even though §2.3
// mandates the non-Try forms throw FormatException on parse failure.
// Convert.TryToInt32 used strtol without a range check, so "9999999999"
// (> INT32_MAX) was silently truncated and returned ok=true.
//
// Fix: all three To* methods now use strtoll/strtod with endptr validation
// (reject empty / leading-whitespace / trailing-garbage → FormatException);
// ToInt32 also range-checks to [INT32_MIN, INT32_MAX] → OverflowException.
// TryToInt32 adds a range check and uses CreateSelect (0 on failure) matching
// int.TryParse (#1227 / §9.1).
// ===========================================================================

// --- Convert.ToInt32: invalid input must throw (§2.3 / #1238) ---
// Before the fix: atoi("not a number") → 0 silently; no exception.
BEZEL_TEST(convert_strict_1238, toint32_invalid_throws_spec_2_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  try "
        "    var n := Convert.ToInt32(\"not a number\"); "
        "    return 99; "
        "  catch Exception => return 0; "
        "  end; "
        "end Main; end C;"), 0);
}

// --- Convert.ToInt32: empty string must throw (§2.3 / #1238) ---
BEZEL_TEST(convert_strict_1238, toint32_empty_throws_spec_2_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  try "
        "    var n := Convert.ToInt32(\"\"); "
        "    return 99; "
        "  catch Exception => return 0; "
        "  end; "
        "end Main; end C;"), 0);
}

// --- Convert.ToInt32: trailing garbage must throw (§2.3 / #1238) ---
// "12abc" is not a valid integer — FormatException expected.
BEZEL_TEST(convert_strict_1238, toint32_trailing_garbage_throws_spec_2_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  try "
        "    var n := Convert.ToInt32(\"12abc\"); "
        "    return 99; "
        "  catch Exception => return 0; "
        "  end; "
        "end Main; end C;"), 0);
}

// --- Convert.ToInt32: valid input still works — regression (§2.3 / #1238) ---
BEZEL_TEST(convert_strict_1238, toint32_valid_regression_spec_2_3) {
    BEZEL_EXPECT_EQ(run(mainBody("return Convert.ToInt32(\"42\");")), 42);
}

// --- Convert.ToInt64: invalid input must throw (§2.3 / #1238) ---
// Before the fix: atoll("bad") → 0 silently.
BEZEL_TEST(convert_strict_1238, toint64_invalid_throws_spec_2_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  try "
        "    var n := Convert.ToInt64(\"bad\"); "
        "    return 99; "
        "  catch Exception => return 0; "
        "  end; "
        "end Main; end C;"), 0);
}

// --- Convert.ToInt64: valid large long still works — regression (§2.3 / #1238) ---
// 9999999999 > INT32_MAX but fits in long; result must equal 9999999999L.
BEZEL_TEST(convert_strict_1238, toint64_valid_large_regression_spec_2_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var n := Convert.ToInt64(\"9999999999\"); "
        "  return (if n = 9999999999L then 1 else 0); "
        "end Main; end C;"), 1);
}

// --- Convert.ToDouble: invalid input must throw (§2.3 / #1238) ---
// Before the fix: atof("abc") → 0.0 silently.
BEZEL_TEST(convert_strict_1238, todouble_invalid_throws_spec_2_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  try "
        "    var d := Convert.ToDouble(\"abc\"); "
        "    return 99; "
        "  catch Exception => return 0; "
        "  end; "
        "end Main; end C;"), 0);
}

// --- Convert.ToDouble: valid input still works — regression (§2.3 / #1238) ---
BEZEL_TEST(convert_strict_1238, todouble_valid_regression_spec_2_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d := Convert.ToDouble(\"3.5\"); "
        "  return (if d > 3.4 and d < 3.6 then 1 else 0); "
        "end Main; end C;"), 1);
}

// --- Convert.TryToInt32: overflow (> INT32_MAX) must return false (§2.3 / #1238) ---
// Before the fix: strtol truncated to i32 → ok=true, result=garbage (1410065407).
BEZEL_TEST(convert_strict_1238, tryto_int32_overflow_returns_false_spec_2_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var result: int := 99; "
        "  var ok := Convert.TryToInt32(\"9999999999\", out result); "
        "  return (if ok then 1 else 0); "
        "end Main; end C;"), 0);
}

// --- Convert.TryToInt32: overflow result must be 0 (default) (§2.3 / #1238) ---
BEZEL_TEST(convert_strict_1238, tryto_int32_overflow_result_zero_spec_2_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var result: int := 99; "
        "  var ok := Convert.TryToInt32(\"9999999999\", out result); "
        "  return result; "
        "end Main; end C;"), 0);
}

// --- Convert.TryToInt32: leading whitespace rejected (§2.3 / #1238) ---
// Matches int.TryParse whitespace policy (#1227 / §9.1).
BEZEL_TEST(convert_strict_1238, tryto_int32_leading_space_returns_false_spec_2_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var result: int := 0; "
        "  var ok := Convert.TryToInt32(\" 42\", out result); "
        "  return (if not ok then 1 else 0); "
        "end Main; end C;"), 1);
}

// --- Convert.TryToInt32: valid regression — ok=true, result=42 (§2.3 / #1238) ---
BEZEL_TEST(convert_strict_1238, tryto_int32_valid_regression_spec_2_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r: int := 0; "
        "  var ok := Convert.TryToInt32(\"42\", out r); "
        "  return (if ok and r = 42 then 1 else 0); "
        "end Main; end C;"), 1);
}
