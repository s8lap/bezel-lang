// Pin tests for issue #8245: the COMPOUND assignment `target op= value` of a
// `DateTime` skips stdlib §2.5's tick-domain check on every NON-LOCAL target.
//
// THE DEFECT. The #7777 fix made the DateTime `+` / `-` operators raise
// `OverflowException` when the result leaves `MinValue..MaxValue`, but only
// the LOCAL spelling of the compound form reached that check: sema's #8059
// rewrite lowers a BARE IDENTIFIER's `x op= e` into `x := x op e` (§6.6's
// desugaring), and the rewrite is deliberately limited to bare identifiers --
// §6.6 requires the target to be evaluated exactly once, and a field /
// property / element target's receiver cannot be evaluated twice. Every other
// lvalue kind therefore computed a RAW i64 tick addition and stored the
// out-of-range word.
//
// MEASURED on r30-c1 (this lane's base) -- the issue's own repro:
//
//   field += no raise 3155378976000000000
//   elem += no raise 3155378976000000000
//   local OVF
//
// 3155378976000000000 is `MaxValue.Ticks` + 1 -- a DateTime outside the
// type's own range -- stored with no raise.
//
// THE CONTRACT. stdlib §2.5 (v0.355), verbatim:
//
//   "The `Add*` family, `Add`, `Subtract(span: TimeSpan)`, and
//    `DateTime.FromTicks` raise `OverflowException` when the result falls
//    outside `MinValue..MaxValue`: the condition is a data-dependent overflow
//    of the tick domain -- language spec
//    [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types)'s
//    `OverflowException` class, the fault `TimeSpan`'s arithmetic above
//    already raises -- a band-4 catchable fault, not a band-1 argument
//    defect; the constructors' component validation keeps
//    `ArgumentOutOfRangeException`."
//
//   "`DateTime.MinValue` is `0001-01-01T00:00:00` (`Ticks` 0) and `MaxValue`
//    is the last tick of `9999-12-31` (`Ticks` 3155378975999999999), so every
//    valid set of components names a value between them."
//
// and of the `+`/`-` operators with a TimeSpan ("Equality and `Kind`"):
//
//   "The `Add*` family, `Add`, `Subtract(span)` and the `+`/`-` operators with
//    a `TimeSpan` keep the `DateTime` operand's `Kind`; `Subtract(other)` and
//    `a - b` subtract the two `Ticks` with no normalization."
//
// and spec §6.6 (Compound assignments), verbatim:
//
//   "the statement `x op= e` is equivalent to `x := x op e` with the target
//    evaluated once"
//
// -- so the compound form IS the binary operator's computation and carries the
// binary operator's check.
//
// Every row below is on the CHECKED (ASan/LSan) AOT oracle and pins the
// program's exact stdout. Each catch arm prints a DISTINCT marker, so a row
// that prints "OVF" proves OverflowException itself was raised.
//
// Cite: stdlib §2.5, v0.355; spec §6.6, v0.355; spec §31.4
// (OverflowException), v0.355.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAotChecked;

namespace {

// The catch arms: a DISTINCT marker per exception type, so "OVF" proves
// OverflowException was raised and "OTHER" proves some other fault was.
const std::string kCatch =
    "catch OverflowException => Console.WriteLine(\"OVF\"); "
    "catch Exception => Console.WriteLine(\"OTHER\"); end; ";

// runAotChecked + the VALUE channel: the program's stdout with ONE trailing
// line-ending stripped (runAotOut's rule), or a sentinel when the program was
// rejected / crashed / tripped the sanitizer.
std::string checkedOut(const std::string& src) {
    auto r = runAotChecked(src);
    if (!r.ran) return "<did-not-run>";
    std::string out = r.stdoutText;
    if (!out.empty() && out.back() == '\n') {
        out.pop_back();
        if (!out.empty() && out.back() == '\r') out.pop_back();
    }
    return out;
}

// A program with the given type declarations and a `Main` body.
std::string prog(const std::string& types, const std::string& mainBody) {
    return "module M; " + types +
           "class C public static method Main(): int " + mainBody +
           " return 0; end Main; end C;";
}

// The classes the rows need.
//
//   H    -- a `DateTime` FIELD at MaxValue and a `TimeSpan` field at MaxValue
//   L    -- the same two fields at MinValue
//   Box  -- a `DateTime` field, a BLOCK-FORM PROPERTY over it, and methods
//           that compound on `this.D`
//   Idx  -- a user-defined INDEXER over a `DateTime` field
const std::string kTypes =
    "class H "
    "  public D: DateTime; "
    "  public Ts: TimeSpan; "
    "  public constructor() "
    "    D := DateTime.MaxValue; "
    "    Ts := TimeSpan.MaxValue; "
    "  end; "
    "end H; "
    "class L "
    "  public D: DateTime; "
    "  public Ts: TimeSpan; "
    "  public constructor() "
    "    D := DateTime.MinValue; "
    "    Ts := TimeSpan.MinValue; "
    "  end; "
    "end L; "
    "class Box "
    "  public D: DateTime; "
    "  public constructor(d: DateTime) D := d; end; "
    "  public property P: DateTime "
    "    get => D; "
    "    set => D := value; "
    "  end P; "
    "  public method Bump(s: TimeSpan) "
    "    this.D += s; "
    "  end Bump; "
    "  public method Drop(s: TimeSpan) "
    "    this.D -= s; "
    "  end Drop; "
    "end Box; "
    "class Idx "
    "  private _v: DateTime; "
    "  public constructor(d: DateTime) _v := d; end; "
    "  public property [i: int]: DateTime "
    "    get => _v; "
    "    set => _v := value; "
    "  end; "
    "end Idx; ";

// One tick: the smallest step in the tick domain (100 ns).
const std::string kOneTick = "TimeSpan.FromTicks(1L)";

}  // namespace

// ---------------------------------------------------------------------------
// The lvalue kinds the compound form reaches. Each row compounds on a target
// whose value is ONE TICK OUTSIDE the domain: `MaxValue` + 1 tick for `+=`,
// `MinValue` - 1 tick for `-=`.
// ---------------------------------------------------------------------------

// A FIELD target -- the issue's own repro.
BEZEL_TEST(datetime_compound_overflow_8245, field_add_and_sub_raise_overflow) {
    BEZEL_EXPECT_EQ(checkedOut(prog(kTypes,
        "var s := " + kOneTick + "; "
        "var h := new H(); "
        "var l := new L(); "
        "try h.D += s; Console.WriteLine($\"no raise {h.D.Ticks}\"); " + kCatch +
        "try l.D -= s; Console.WriteLine($\"no raise {l.D.Ticks}\"); " + kCatch)),
        "OVF\nOVF");
}

// An ARRAY ELEMENT target -- the issue's other repro.
BEZEL_TEST(datetime_compound_overflow_8245, array_element_add_and_sub_raise_overflow) {
    BEZEL_EXPECT_EQ(checkedOut(prog(kTypes,
        "var s := " + kOneTick + "; "
        "var a := new Array<DateTime>(1, DateTime.MaxValue); "
        "var b := new Array<DateTime>(1, DateTime.MinValue); "
        "try a[0] += s; Console.WriteLine($\"no raise {a[0].Ticks}\"); " + kCatch +
        "try b[0] -= s; Console.WriteLine($\"no raise {b[0].Ticks}\"); " + kCatch)),
        "OVF\nOVF");
}

// A STATIC FIELD target.
//
// §22.12 rule 2 admits no MUTABLE static of a compiler-known value builtin
// type (`DateTime` is not in the admitted category set), and §22.8 admits a
// `static readonly` field's assignment only inside the declaring type's
// static-constructor window -- so the static target is spelled
// `S.SD -= …` there. The BARE spelling (`SD -= …`) is already checked (sema's
// #8059 rewrite reaches the binary operator for a bare identifier), so the
// qualified spelling is the one that pins the hole.
BEZEL_TEST(datetime_compound_overflow_8245, static_field_sub_raises_overflow) {
    BEZEL_EXPECT_EQ(checkedOut(prog(
        "class S "
        "  public static readonly SD: DateTime; "
        "  static constructor() "
        "    try "
        "      S.SD -= " + kOneTick + "; "
        "      Console.WriteLine(\"no raise\"); "
        "    catch OverflowException => Console.WriteLine(\"OVF\"); "
        "    catch Exception => Console.WriteLine(\"OTHER\"); "
        "    end; "
        "  end; "
        "end S; ",
        "var s := new S(); "
        "Console.WriteLine($\"{S.SD.Ticks}\"); ")),
        "OVF\n0");
}

// A PROPERTY target (a block-form property: getter, operator, setter).
BEZEL_TEST(datetime_compound_overflow_8245, property_add_and_sub_raise_overflow) {
    BEZEL_EXPECT_EQ(checkedOut(prog(kTypes,
        "var s := " + kOneTick + "; "
        "var hi := new Box(DateTime.MaxValue); "
        "var lo := new Box(DateTime.MinValue); "
        "try hi.P += s; Console.WriteLine($\"no raise {hi.P.Ticks}\"); " + kCatch +
        "try lo.P -= s; Console.WriteLine($\"no raise {lo.P.Ticks}\"); " + kCatch)),
        "OVF\nOVF");
}

// An INDEXER target (a user-defined indexer's slot).
BEZEL_TEST(datetime_compound_overflow_8245, indexer_add_and_sub_raise_overflow) {
    BEZEL_EXPECT_EQ(checkedOut(prog(kTypes,
        "var s := " + kOneTick + "; "
        "var hi := new Idx(DateTime.MaxValue); "
        "var lo := new Idx(DateTime.MinValue); "
        "try hi[0] += s; Console.WriteLine($\"no raise {hi[0].Ticks}\"); " + kCatch +
        "try lo[0] -= s; Console.WriteLine($\"no raise {lo[0].Ticks}\"); " + kCatch)),
        "OVF\nOVF");
}

// A `this.F` target -- the compound form inside a method, on the receiver's
// own field.
BEZEL_TEST(datetime_compound_overflow_8245, this_field_add_and_sub_raise_overflow) {
    BEZEL_EXPECT_EQ(checkedOut(prog(kTypes,
        "var s := " + kOneTick + "; "
        "var hi := new Box(DateTime.MaxValue); "
        "var lo := new Box(DateTime.MinValue); "
        "try hi.Bump(s); Console.WriteLine($\"no raise {hi.D.Ticks}\"); " + kCatch +
        "try lo.Drop(s); Console.WriteLine($\"no raise {lo.D.Ticks}\"); " + kCatch)),
        "OVF\nOVF");
}

// ---------------------------------------------------------------------------
// The TimeSpan row. stdlib §2.5 says of the DateTime members: "the fault
// `TimeSpan`'s arithmetic above already raises" -- and TimeSpan's own
// `operator +(a, b: TimeSpan)` / `operator -(a, b: TimeSpan)` are declared
// over the tick count, whose domain is the whole signed 64-bit range
// (`MinValue` is `FromTicks(long.MinValue)`, `MaxValue` is
// `FromTicks(long.MaxValue)`). So a TimeSpan compound assignment one tick
// past an end of that domain raises OverflowException. This row is a
// CONTROL: it already raised before the fix (the tick domain IS the i64
// range, so the ordinary checked integer arithmetic enforces it) and must
// keep raising.
// ---------------------------------------------------------------------------
BEZEL_TEST(datetime_compound_overflow_8245, timespan_field_add_and_sub_raise_overflow) {
    BEZEL_EXPECT_EQ(checkedOut(prog(kTypes,
        "var s := " + kOneTick + "; "
        "var h := new H(); "
        "var l := new L(); "
        "try h.Ts += s; Console.WriteLine($\"no raise {h.Ts.Ticks}\"); " + kCatch +
        "try l.Ts -= s; Console.WriteLine($\"no raise {l.Ts.Ticks}\"); " + kCatch)),
        "OVF\nOVF");
}

// ---------------------------------------------------------------------------
// Controls: an in-range compound assignment computes the exact value.
// ---------------------------------------------------------------------------

// The in-range FIELD compound assignment: the new tick count, exactly.
BEZEL_TEST(datetime_compound_overflow_8245, field_add_in_range_is_exact) {
    BEZEL_EXPECT_EQ(checkedOut(prog(kTypes,
        "var s := " + kOneTick + "; "
        "var h := new H(); "
        "h.D := new DateTime(2024, 1, 1); "
        "h.D += s; "
        "Console.WriteLine($\"{h.D.Ticks}\"); ")),
        "638396640000000001");
}

// The in-range FIELD compound subtraction: the new tick count, exactly.
BEZEL_TEST(datetime_compound_overflow_8245, field_sub_in_range_is_exact) {
    BEZEL_EXPECT_EQ(checkedOut(prog(kTypes,
        "var s := " + kOneTick + "; "
        "var h := new H(); "
        "h.D := new DateTime(2024, 1, 1); "
        "h.D -= s; "
        "Console.WriteLine($\"{h.D.Ticks}\"); ")),
        "638396639999999999");
}

// The in-range ARRAY-ELEMENT compound assignment: the new tick count,
// exactly -- the write-back, not just the arithmetic, is exercised.
BEZEL_TEST(datetime_compound_overflow_8245, array_element_add_in_range_is_exact) {
    BEZEL_EXPECT_EQ(checkedOut(prog(kTypes,
        "var s := " + kOneTick + "; "
        "var a := new Array<DateTime>(1, new DateTime(2024, 1, 1)); "
        "a[0] += s; "
        "Console.WriteLine($\"{a[0].Ticks}\"); ")),
        "638396640000000001");
}

// The LOCAL spelling -- the one the #7777 fix already reached -- must keep
// raising, and must keep computing the exact value in range.
BEZEL_TEST(datetime_compound_overflow_8245, local_add_raises_overflow) {
    BEZEL_EXPECT_EQ(checkedOut(prog(kTypes,
        "var s := " + kOneTick + "; "
        "var d := DateTime.MaxValue; "
        "try d += s; Console.WriteLine(\"no raise\"); " + kCatch)),
        "OVF");
}

BEZEL_TEST(datetime_compound_overflow_8245, local_add_in_range_is_exact) {
    BEZEL_EXPECT_EQ(checkedOut(prog(kTypes,
        "var s := " + kOneTick + "; "
        "var d := new DateTime(2024, 1, 1); "
        "d += s; "
        "Console.WriteLine($\"{d.Ticks}\"); ")),
        "638396640000000001");
}

// The compound form keeps the `DateTime` operand's `Kind` (stdlib §2.5, "the
// `+`/`-` operators with a `TimeSpan` keep the `DateTime` operand's `Kind`"):
// a `Kind = Utc` target stays `Utc` across the compound assignment.
BEZEL_TEST(datetime_compound_overflow_8245, compound_keeps_the_kind) {
    BEZEL_EXPECT_EQ(checkedOut(prog(kTypes,
        "var s := " + kOneTick + "; "
        "var h := new H(); "
        "h.D := new DateTime(2024, 1, 1).ToUtc(); "
        "h.D += s; "
        "Console.WriteLine(h.D.Kind = DateTimeKind.Utc); ")),
        "True");
}

// The NULLABLE (`DateTime?`) spelling -- §7.11 lifts the operator over a
// nullable operand, and "the runtime checks each operator already performs
// -- `OverflowException` on integer overflow (§6.1) -- apply only when both
// operands are present (non-null)". Both operands are present here, so the
// §2.5 tick-domain check applies, and the compound form raises.
BEZEL_TEST(datetime_compound_overflow_8245, nullable_local_add_raises_overflow) {
    BEZEL_EXPECT_EQ(checkedOut(prog(kTypes,
        "var s := " + kOneTick + "; "
        "var d: DateTime? := DateTime.MaxValue; "
        "try d += s; Console.WriteLine(\"no raise\"); " + kCatch)),
        "OVF");
}
