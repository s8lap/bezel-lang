// Regression tests for stdlib §5/§5.1 + §2.19 and language spec §6.1 (v0.307):
// the no-selector `Sum()`/`Min()`/`Max()` aggregations over a NARROW integer
// element type (`byte`, `sbyte`, `short`, `ushort`) — issue #4695.
//
// stdlib §2.19 names the §2.19 `INumber<T>` conformers exactly:
//
//     byte, sbyte, short, ushort, int, uint, long, ulong, float, double, decimal
//
// and stdlib §5.1 declares the no-selector form over that constraint:
//
//     public extension IEnumerable<T> where T: INumber<T>
//       public method Sum(): owned T;
//     end;
//
// stdlib §5 states the consequence normatively: "`Sum` now returns the
// element's own type, so a narrow integer element type accumulates in that type
// and is overflow-checked in it (spec §6.1) — `bytes.Sum()` over an
// `Array<byte>` raises `OverflowException` past 255."
//
// Fix #1836 keyed only SEVEN of the eleven conformers (int/uint/long/ulong/
// float/double/decimal), so the four narrow integers stayed typed `int` in sema
// while codegen still loaded the element at its own width. The reconciliation
// step in the `Sum` / `Min` / `Max` array terminals was an unconditional
//
//     val = builder_.CreateTrunc(val, accLLTy);
//
// i.e. a WIDENING spelled as a truncation. LLVM's verifier rejects it, so the
// whole family failed to compile:
//
//     DestTy too big for Trunc
//       %19 = trunc i8 %18 to i32
//     bezelc: codegen error: IR verification failed (malformed IR emitted)
//
// Every row below therefore fails on the pre-fix compiler by never producing a
// program at all — `runAotOut` returns its "<did-not-run>" sentinel. The oracle
// is the PROGRAM'S STDOUT, not an exit code: exit 1 is also AddressSanitizer's
// abort status (so an exit-only check would be vacuous under the gate's
// `--full` leg), and `failedAtCompileTimeWithRealLocation` would answer true on
// both sides of the fix because the pre-fix behaviour is a compiler abort whose
// own backstop diagnostic carries a location.
//
// SIGNEDNESS IS THE LOAD-BEARING PART. `byte` and `sbyte` are both i8 and must
// overflow at different points — 100 + 100 = 200 is in range for `byte` and out
// of range for `sbyte` — so the checked-add intrinsic's signedness (`uadd` vs
// `sadd`) and `Min`/`Max`'s comparison predicate (`icmp ult` vs `icmp slt`) are
// each a real fork. The signed/unsigned pairs below are chosen so a fix that
// used one instruction for both would fail at least one row in each pair.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

namespace {

// Wrap a Main body in the minimal module/class shell the AOT harness compiles.
std::string prog(const std::string& body) {
    return "module M; class C "
           "  public static method Main(): int "
           + body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// ── Sum(): the accumulator is the element's own type ────────────────────────

// stdlib §5.1 / §2.19, v0.307: `Array<byte>.Sum(): byte`. 200 + 50 = 250 fits
// in `byte`, so the sum is exact and no exception is raised. Pre-fix this did
// not compile ("DestTy too big for Trunc").
BEZEL_TEST(narrow_element_aggregation_4695, sum_byte_in_range) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    using var a: Array<byte> := [byte(200), byte(50)]; "
        "    Console.WriteLine(a.Sum()); ")), "250");
}

// stdlib §5.1 / §2.19, v0.307: `Array<byte>.Sum(): byte` at the exact upper
// bound. 200 + 55 = 255 is byte.MaxValue — in range. This row is the
// UNSIGNEDNESS oracle: under a signed `sadd.with.overflow.i8` the accumulator
// would already have "overflowed" at 128, so a fix that used the signed
// intrinsic for `byte` fails here while `sum_byte_in_range` might still pass.
BEZEL_TEST(narrow_element_aggregation_4695, sum_byte_at_max_value) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    using var a: Array<byte> := [byte(200), byte(55)]; "
        "    Console.WriteLine(a.Sum()); ")), "255");
}

// stdlib §5 (v0.307), the rule's own worked example: "bytes.Sum() over an
// Array<byte> raises OverflowException past 255". 200 + 200 = 400. This is the
// exact repro from rule-corpus row l5_0012.
BEZEL_TEST(narrow_element_aggregation_4695, sum_byte_overflows_past_255) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    using var a: Array<byte> := [byte(200), byte(200)]; "
        "    try "
        "      Console.WriteLine(a.Sum()); "
        "    catch "
        "      OverflowException => Console.WriteLine(\"caught\") "
        "    end; ")), "caught");
}

// stdlib §5.1 / §6.1, v0.307: `Array<sbyte>.Sum(): sbyte`. 100 + 100 = 200 is
// OUTSIDE sbyte's -128..127 range, so §6.1's checked `+` raises. This is the
// SIGNEDNESS oracle for the same two element values that are perfectly in range
// for `byte` above: one intrinsic cannot satisfy both rows.
BEZEL_TEST(narrow_element_aggregation_4695, sum_sbyte_overflows_past_127) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    using var a: Array<sbyte> := [sbyte(100), sbyte(100)]; "
        "    try "
        "      Console.WriteLine(a.Sum()); "
        "    catch "
        "      OverflowException => Console.WriteLine(\"caught\") "
        "    end; ")), "caught");
}

// stdlib §5.1 / §6.1, v0.307: the negative direction of the same rule —
// -100 + -100 = -200 is below sbyte.MinValue (-128), so it raises. An UNSIGNED
// intrinsic would not report this, so the row pins `sadd` for `sbyte`.
BEZEL_TEST(narrow_element_aggregation_4695, sum_sbyte_overflows_below_min) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    using var a: Array<sbyte> := [sbyte(-100), sbyte(-100)]; "
        "    try "
        "      Console.WriteLine(a.Sum()); "
        "    catch "
        "      OverflowException => Console.WriteLine(\"caught\") "
        "    end; ")), "caught");
}

// stdlib §5.1 / §2.19, v0.307: `Array<sbyte>.Sum(): sbyte`, in range and
// signed. -100 + 50 = -50 — a correct negative total requires the element to be
// read as signed and the accumulator to be sbyte-wide.
BEZEL_TEST(narrow_element_aggregation_4695, sum_sbyte_negative_in_range) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    using var a: Array<sbyte> := [sbyte(-100), sbyte(50)]; "
        "    Console.WriteLine(a.Sum()); ")), "-50");
}

// stdlib §5.1 / §2.19, v0.307: `Array<short>.Sum(): short`. 30000 + 2000 =
// 32000 is inside short's 16-bit signed range. i16 is the second affected
// width — the defect is per-width, not specific to i8.
BEZEL_TEST(narrow_element_aggregation_4695, sum_short_in_range) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    using var a: Array<short> := [short(30000), short(2000)]; "
        "    Console.WriteLine(a.Sum()); ")), "32000");
}

// stdlib §5.1 / §6.1, v0.307: 30000 + 3000 = 33000 exceeds short.MaxValue
// (32767), so §6.1's checked `+` raises at the short width. Pre-#4695 the sum
// would have accumulated in `int` and quietly produced 33000 had the IR been
// valid at all.
BEZEL_TEST(narrow_element_aggregation_4695, sum_short_overflows_past_32767) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    using var a: Array<short> := [short(30000), short(3000)]; "
        "    try "
        "      Console.WriteLine(a.Sum()); "
        "    catch "
        "      OverflowException => Console.WriteLine(\"caught\") "
        "    end; ")), "caught");
}

// stdlib §5.1 / §2.19, v0.307: `Array<ushort>.Sum(): ushort`. 60000 + 5000 =
// 65000 fits in ushort (max 65535) but NOT in a signed i16 — the unsigned
// counterpart of `sum_byte_at_max_value` at the 16-bit width.
BEZEL_TEST(narrow_element_aggregation_4695, sum_ushort_in_range_above_int16) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    using var a: Array<ushort> := [ushort(60000), ushort(5000)]; "
        "    Console.WriteLine(a.Sum()); ")), "65000");
}

// stdlib §5.1 / §6.1, v0.307: 60000 + 6000 = 66000 exceeds ushort.MaxValue
// (65535) and raises.
BEZEL_TEST(narrow_element_aggregation_4695, sum_ushort_overflows_past_65535) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    using var a: Array<ushort> := [ushort(60000), ushort(6000)]; "
        "    try "
        "      Console.WriteLine(a.Sum()); "
        "    catch "
        "      OverflowException => Console.WriteLine(\"caught\") "
        "    end; ")), "caught");
}

// ── Min() / Max(): the same accumulator, and the same signedness fork ───────

// stdlib §5.1, v0.307: `Min()`/`Max()` over `IEnumerable<T> where T:
// IComparable<T>` return `T`. `Array<byte>` shares the broken reconciliation
// step, so this did not compile pre-fix either. 200 > 50 UNSIGNED; read as
// signed i8, 200 is -56 and the answers invert — so this row pins `icmp ult`
// for `byte` as well as the i8-wide accumulator.
BEZEL_TEST(narrow_element_aggregation_4695, min_max_byte_unsigned_order) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    using var a: Array<byte> := [byte(200), byte(50)]; "
        "    Console.WriteLine(a.Min()); "
        "    Console.WriteLine(a.Max()); ")), "50\n200");
}

// stdlib §5.1, v0.307: the signed counterpart — `Array<sbyte>` must order by
// the SIGNED value, so Min is -100 and Max is 100. A fix that used the unsigned
// predicate for every narrow integer would invert this pair.
BEZEL_TEST(narrow_element_aggregation_4695, min_max_sbyte_signed_order) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    using var a: Array<sbyte> := [sbyte(-100), sbyte(100)]; "
        "    Console.WriteLine(a.Min()); "
        "    Console.WriteLine(a.Max()); ")), "-100\n100");
}

// stdlib §5.1, v0.307: the 16-bit unsigned width. 60000 > 1 unsigned; as signed
// i16, 60000 is -5536 and the pair inverts.
BEZEL_TEST(narrow_element_aggregation_4695, min_max_ushort_unsigned_order) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    using var a: Array<ushort> := [ushort(60000), ushort(1)]; "
        "    Console.WriteLine(a.Min()); "
        "    Console.WriteLine(a.Max()); ")), "1\n60000");
}

// stdlib §5.1, v0.307: the 16-bit signed width, for completeness of the table.
BEZEL_TEST(narrow_element_aggregation_4695, min_max_short_signed_order) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    using var a: Array<short> := [short(-30000), short(30000)]; "
        "    Console.WriteLine(a.Min()); "
        "    Console.WriteLine(a.Max()); ")), "-30000\n30000");
}

// ── The lazy receiver reaches the SAME array terminal ───────────────────────

// stdlib §5/§5.1, v0.307: a lazy `Where(...)` source materializes into an
// Array<byte> and runs the identical terminal, so the narrow-element rule must
// hold there too. 10 + 20 + 30 = 60, in range for `byte`.
BEZEL_TEST(narrow_element_aggregation_4695, sum_byte_lazy_receiver) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    using var a: Array<byte> := [byte(10), byte(20), byte(30)]; "
        "    Console.WriteLine(a.Where(lambda(x) => x > byte(5)).Sum()); ")),
        "60");
}

// ── The empty source still returns the zero of the RESULT type ──────────────

// stdlib §5.1, v0.307: "On an empty source: Sum (both forms, §5.1's
// no-selector included) returns the zero of its result type (T.Zero())." The
// result type is now `byte`, so the answer is byte's zero — and the checked
// accumulation must not be reached at all.
BEZEL_TEST(narrow_element_aggregation_4695, sum_byte_empty_is_zero) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    using var a := new Array<byte>(); "
        "    Console.WriteLine(a.Sum()); ")), "0");
}

// ── The already-working widths must not regress ─────────────────────────────

// stdlib §5.1, v0.226/#1836: `Array<long>.Sum(): long` — the seven conformers
// #1836 already keyed keep working unchanged. Guards the shared switch this fix
// extends: 3e9 + 6e9 = 9e9 needs the 64-bit accumulator.
BEZEL_TEST(narrow_element_aggregation_4695, sum_long_unchanged) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    using var a := new Array<long>(); "
        "    a.Add(3000000000L); a.Add(6000000000L); "
        "    Console.WriteLine(a.Sum()); ")), "9000000000");
}

// language spec §6.1, v0.307: integer `+` is checked at EVERY width, so the
// `int` accumulator raises too — 2e9 + 2e9 = 4e9 exceeds int.MaxValue. This
// pins that extending the checked-add to the whole integer family (rather than
// only the narrow widths) is deliberate, and that the `int` path selects the
// SIGNED intrinsic.
BEZEL_TEST(narrow_element_aggregation_4695, sum_int_overflow_is_checked) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    using var a := new Array<int>(); "
        "    a.Add(2000000000); a.Add(2000000000); "
        "    try "
        "      Console.WriteLine(a.Sum()); "
        "    catch "
        "      OverflowException => Console.WriteLine(\"caught\") "
        "    end; ")), "caught");
}
