// Spec-derived regression test for issue #7692 — a NARROWED nullable value
// passed to a built-in CONSTRUCTOR (`DateTime`, `DateOnly`, `TimeOnly`) or a
// nullable RECORD passed to a built-in MEMBER parameter (`DateTime.Add(span:
// TimeSpan)`), and the UN-narrowed twins (campaign nullable-carrier, lane
// C2-29). Extends fix #7621 (builtin_narrowed_arg_7621_test.cpp).
//
// ─── THE NORMATIVE SENTENCES (CHECKED in docs/, v0.350) ─────────────────────
//
// stdlib §2.5 declares `DateTime(year: int, month: int, day: int)`,
//   `DateTime(year, month, day, hour, minute, second: int, millisecond: int
//   := 0, kind: DateTimeKind := DateTimeKind.Unspecified)`, `DateOnly(year:
//   int, month: int, day: int)`, `TimeOnly(hour: int, minute: int, second:
//   int := 0, millisecond: int := 0)`, `DateTime.Add(span: TimeSpan)`,
//   `Subtract(other: DateTime)` / `Subtract(span: TimeSpan)`, `CompareTo(other:
//   T)` and `DateOnly.ToDateTime(time: TimeOnly)`: every parameter is
//   non-nullable.
// spec §5.1 / §7 / §7.3: a `T?` reaches no non-nullable parameter; an
//   un-narrowed nullable there is a compile error ("the argument ... is
//   nullable but the target type is non-nullable"). `!` (§7.8) and `??`
//   (§7.7) are the remedies.
// spec §7.4: a declaration's own assignment, a null test and a `readonly`
//   field path narrow. spec §22.6: "a read of a binding §7.4 has narrowed
//   non-null is of its non-nullable type", so the narrowed read is a LEGAL
//   argument and evaluates as the value it denotes.
//
// ─── THE DEFECT (MEASURED on staging 87a31a36) ──────────────────────────────
//
// `var y: int? := 2021; new DateTime(y, 1, 1)`, `new TimeOnly(h, m)`, `new
// DateOnly(2020, h, 3)` and `dt.Add(ts)` over a `ts: TimeSpan?` aborted the
// compiler in the LLVM verifier ("IR verification failed"): the constructor
// table carried nonNullable mask 0 (so the read was never published for
// payload extraction) and the member gate published only Primitive nullables.
// The UN-narrowed `new DateTime(y, 1, 1)` crashed the same way instead of
// being the §7 diagnostic.
//
// The oracle is the program's OUTPUT (accept rows) or a located §7 diagnostic
// (refuse rows).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::diagnosticMessageContains;

namespace {

std::string prog(const std::string& body) {
    return "module M; import System; import System.Text; "
           "class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// A program whose nullable PARAMETER `x` is never narrowed (§7.4).
std::string paramProg(const std::string& type, const std::string& body) {
    return "module M; import System; import System.Text; "
           "class C "
           "  public static method Main(): int "
           "    return F(null); "
           "  end Main; "
           "  public static method F(x: " + type + "): int " +
           body +
           "    return 0; "
           "  end F; "
           "end C;";
}

// A nullable PARAMETER `x` called with a real value, so `x!` / `x ?? v` run.
std::string paramRun(const std::string& type, const std::string& arg,
                     const std::string& body) {
    return "module M; import System; import System.Text; "
           "class C "
           "  public static method Main(): int "
           "    return F(" + arg + "); "
           "  end Main; "
           "  public static method F(x: " + type + "): int " +
           body +
           "    return 0; "
           "  end F; "
           "end C;";
}

// `callee` is the text between "the argument to " and " is nullable": a
// member is "`DateTime.Add`", a constructor "`DateTime` constructor".
void expectRefused(const std::string& type, const std::string& stmt,
                   const std::string& callee) {
    auto r = runAot(paramProg(type, stmt));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, ("the argument to " + callee +
            " is nullable but the target type is non-nullable").c_str()));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
}

void expectNullableRefused(const std::string& type, const std::string& stmt,
                           const char* callee) {
    expectRefused(type, stmt, std::string("`") + callee + "`");
}

void expectCtorRefused(const std::string& type, const std::string& stmt,
                       const char* head) {
    expectRefused(type, stmt, std::string("`") + head + "` constructor");
}

}  // namespace

// ===========================================================================
// ACCEPT: a §7.4-narrowed read is the non-nullable type it denotes (§22.6).
// ===========================================================================

// The issue's s04: a narrowed `int?` at `new DateTime(y, 1, 1)`.
BEZEL_TEST(builtin_narrowed_ctor_7692, datetime_ctor_three_args_narrowed) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var y: int? := 2021; var dt := new DateTime(y, 1, 1); "
        "    Console.WriteLine(dt.Year);")),
        "2021");
}

// The issue's s08: `TimeOnly(hour, minute)` and `DateOnly(2020, h, 3)`.
BEZEL_TEST(builtin_narrowed_ctor_7692, timeonly_dateonly_ctor_narrowed) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var h: int? := 5; var m: int? := 6; "
        "    var t := new TimeOnly(h, m); Console.WriteLine(t.Hour); "
        "    Console.WriteLine(t.Minute); "
        "    var dd := new DateOnly(2020, h, 3); Console.WriteLine(dd.Month);")),
        "5\n6\n5");
}

// The default-bearing forms: `TimeOnly` at three and four arguments, and the
// `DateTime` six-, seven- and eight-argument forms (stdlib §2.5).
BEZEL_TEST(builtin_narrowed_ctor_7692, timeonly_three_and_four_args_narrowed) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var s: int? := 7; var ms: int? := 8; "
        "    var a := new TimeOnly(1, 2, s); Console.WriteLine(a.Second); "
        "    var b := new TimeOnly(1, 2, 3, ms); "
        "    Console.WriteLine(b.Millisecond);")),
        "7\n8");
}

BEZEL_TEST(builtin_narrowed_ctor_7692, datetime_six_seven_eight_args_narrowed) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var h: int? := 4; var ms: int? := 9; "
        "    var k: DateTimeKind? := DateTimeKind.Utc; "
        "    var a := new DateTime(2020, 1, 2, h, 5, 6); "
        "    Console.WriteLine(a.Hour); "
        "    var b := new DateTime(2020, 1, 2, 3, 5, 6, ms); "
        "    Console.WriteLine(b.Millisecond); "
        "    var c := new DateTime(2020, 1, 2, 3, 5, 6, 0, k); "
        "    Console.WriteLine(c.Year);")),
        "4\n9\n2020");
}

// A narrowed read at EVERY position of one construction.
BEZEL_TEST(builtin_narrowed_ctor_7692, datetime_ctor_every_position_narrowed) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var y: int? := 2001; var mo: int? := 2; var d: int? := 3; "
        "    var h: int? := 4; var mi: int? := 5; var s: int? := 6; "
        "    var dt := new DateTime(y, mo, d, h, mi, s); "
        "    Console.WriteLine(dt.ToString());")),
        "2001-02-03T04:05:06");
}

// A null test narrows too (§7.4), here a parameter inside the guard.
BEZEL_TEST(builtin_narrowed_ctor_7692, datetime_ctor_null_test_narrowed) {
    BEZEL_EXPECT_EQ(runAotOut(paramRun("int?", "1999",
        "    if x <> null then "
        "      var dt := new DateTime(x, 12, 31); Console.WriteLine(dt.Year); "
        "    end; ")),
        "1999");
}

// The remedies spell the non-nullable type: `x!` (§7.8) and `x ?? v` (§7.7).
BEZEL_TEST(builtin_narrowed_ctor_7692, datetime_ctor_bang_and_coalesce) {
    BEZEL_EXPECT_EQ(runAotOut(paramRun("int?", "2030",
        "    var a := new DateTime(x!, 1, 1); Console.WriteLine(a.Year); "
        "    var b := new DateTime(x ?? 1, 1, 1); Console.WriteLine(b.Year); "
        "    var c := new TimeOnly(x! mod 24, 0); "
        "    Console.WriteLine(c.Hour); ")),
        "2030\n2030\n14");
}

// §22.5 named arguments at a built-in constructor: the in-order and the
// REORDERED binding both read the parameter each name binds.
BEZEL_TEST(builtin_narrowed_ctor_7692, datetime_ctor_named_args_narrowed) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var y: int? := 2021; var m: int? := 7; "
        "    var a := new DateTime(year := y, month := 3, day := 4); "
        "    Console.WriteLine(a.ToString()); "
        "    var b := new DateTime(year := 2000, day := 4, month := m); "
        "    Console.WriteLine(b.ToString()); "
        "    var t := new TimeOnly(minute := m, hour := 1); "
        "    Console.WriteLine(t.ToString());")),
        "2021-03-04T00:00:00\n2000-07-04T00:00:00\n01:07:00");
}

// A `readonly` field path narrows (§7.4): the read of `this._y` is of `int`.
BEZEL_TEST(builtin_narrowed_ctor_7692, datetime_ctor_readonly_field_narrowed) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System; "
        "class H public readonly _y: int?; public readonly _t: TimeSpan?; "
        "  public constructor(y: int?, t: TimeSpan?) "
        "    this._y := y; this._t := t; end constructor; "
        "  destructor end destructor; "
        "  public method Show() "
        "    if _y <> null then "
        "      var dt := new DateTime(_y, 1, 1); Console.WriteLine(dt.Year); "
        "      if _t <> null then "
        "        Console.WriteLine(dt.Add(_t).Minute); end; "
        "    end; "
        "  end Show; "
        "  public static method Main(): int "
        "    using var h := new H(2044, TimeSpan.FromMinutes(5.0)); "
        "    h.Show(); return 0; "
        "  end Main; end H;"),
        "2044\n5");
}

// The record twin, #7692's s02: a narrowed `TimeSpan?` at `Add(span)`.
BEZEL_TEST(builtin_narrowed_ctor_7692, datetime_add_record_narrowed) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var ts: TimeSpan? := TimeSpan.FromSeconds(90.0); "
        "    var dt := new DateTime(2020, 1, 1); "
        "    Console.WriteLine(dt.Add(ts).Minute); "
        "    Console.WriteLine(dt.Subtract(ts).Day);")),
        "1\n31");
}

BEZEL_TEST(builtin_narrowed_ctor_7692, datetime_subtract_compare_record_narrowed) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var d2: DateTime? := new DateTime(2020, 1, 2); "
        "    var dt := new DateTime(2020, 1, 1); "
        "    Console.WriteLine(dt.Subtract(d2).Days); "
        "    Console.WriteLine(dt.CompareTo(d2)); "
        "    var t1: TimeSpan? := TimeSpan.FromSeconds(1.0); "
        "    Console.WriteLine(TimeSpan.Zero.CompareTo(t1)); "
        "    var o1: DateOnly? := new DateOnly(2020, 1, 1); "
        "    Console.WriteLine(new DateOnly(2020, 1, 1).CompareTo(o1)); "
        "    var m1: TimeOnly? := new TimeOnly(1, 2); "
        "    Console.WriteLine(new TimeOnly(1, 2).CompareTo(m1));")),
        "-1\n-1\n-1\n0\n0");
}

BEZEL_TEST(builtin_narrowed_ctor_7692, dateonly_to_datetime_record_narrowed) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var tm: TimeOnly? := new TimeOnly(1, 2); "
        "    var dd := new DateOnly(2020, 1, 1); "
        "    Console.WriteLine(dd.ToDateTime(tm).Hour);")),
        "1");
}

BEZEL_TEST(builtin_narrowed_ctor_7692, datetime_add_record_bang_and_coalesce) {
    BEZEL_EXPECT_EQ(runAotOut(paramRun("TimeSpan?", "TimeSpan.FromSeconds(90.0)",
        "    var dt := new DateTime(2020, 1, 1); "
        "    Console.WriteLine(dt.Add(x!).Minute); "
        "    Console.WriteLine(dt.Add(x ?? TimeSpan.Zero).Minute); ")),
        "1\n1");
}

// Control: `StringBuilder(capacity)` (a Primitive nullable at a constructor
// with a mask) and `StringBuilder(initial)` already evaluated; they stay so.
BEZEL_TEST(builtin_narrowed_ctor_7692, stringbuilder_ctor_narrowed_control) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var cap: int? := 16; var s: string? := \"ab\"; "
        "    var sb := new StringBuilder(cap); sb.Append(\"x\"); "
        "    Console.WriteLine(sb.ToString()); "
        "    var sb2 := new StringBuilder(s); "
        "    Console.WriteLine(sb2.ToString());")),
        "x\nab");
}

// ===========================================================================
// REFUSE: an UN-narrowed nullable is the §7 diagnostic, never a crash.
// ===========================================================================

// The issue's s03.
BEZEL_TEST(builtin_narrowed_ctor_7692, datetime_ctor_unnarrowed_refused) {
    expectCtorRefused("int?",
        "    var dt := new DateTime(x, 1, 1); Console.WriteLine(dt.Year);", "DateTime");
}

BEZEL_TEST(builtin_narrowed_ctor_7692, datetime_ctor_other_positions_refused) {
    expectCtorRefused("int?",
        "    var dt := new DateTime(2020, 1, 1, x, 1, 1);", "DateTime");
    expectCtorRefused("int?",
        "    var dt := new DateTime(2020, 1, 1, 1, 1, 1, x);", "DateTime");
    expectCtorRefused("DateTimeKind?",
        "    var dt := new DateTime(2020, 1, 1, 1, 1, 1, 0, x);", "DateTime");
}

BEZEL_TEST(builtin_narrowed_ctor_7692, dateonly_timeonly_ctor_unnarrowed_refused) {
    expectCtorRefused("int?",
        "    var d := new DateOnly(2020, x, 5);", "DateOnly");
    expectCtorRefused("int?",
        "    var t := new TimeOnly(x, 5);", "TimeOnly");
    expectCtorRefused("int?",
        "    var t := new TimeOnly(1, 5, x);", "TimeOnly");
    expectCtorRefused("int?",
        "    var t := new TimeOnly(1, 5, 1, x);", "TimeOnly");
}

// A reassigned-from-null local is not narrowed (§7.4).
BEZEL_TEST(builtin_narrowed_ctor_7692, datetime_ctor_reassigned_null_refused) {
    auto r = runAot(prog(
        "    var y: int? := 2021; y := null; var dt := new DateTime(y, 1, 1);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument to `DateTime` constructor is nullable but the target "
           "type is non-nullable"));
}

BEZEL_TEST(builtin_narrowed_ctor_7692, datetime_ctor_named_unnarrowed_refused) {
    expectCtorRefused("int?",
        "    var dt := new DateTime(year := x, month := 1, day := 1);", "DateTime");
    expectCtorRefused("int?",
        "    var dt := new DateTime(year := 2000, day := 1, month := x);", "DateTime");
}

BEZEL_TEST(builtin_narrowed_ctor_7692, record_param_unnarrowed_refused) {
    expectNullableRefused("TimeSpan?",
        "    var dt := new DateTime(2020, 1, 1); Console.WriteLine(dt.Add(x).Day);",
        "DateTime.Add");
    expectNullableRefused("TimeSpan?",
        "    var dt := new DateTime(2020, 1, 1); "
        "    Console.WriteLine(dt.Subtract(x).Day);", "DateTime.Subtract");
    expectNullableRefused("DateTime?",
        "    var dt := new DateTime(2020, 1, 1); "
        "    Console.WriteLine(dt.CompareTo(x));", "DateTime.CompareTo");
    expectNullableRefused("TimeOnly?",
        "    var dd := new DateOnly(2020, 1, 1); "
        "    Console.WriteLine(dd.ToDateTime(x).Hour);", "DateOnly.ToDateTime");
}

// ===========================================================================
// `StringBuilder.Append` over a narrowed nullable VALUE of a user / built-in
// Named type (review of lane C2-29, fix-forward C2-29b).
//
// stdlib §3.1 declares `Append(value: string)` / `Append(value: Object)`, both
// non-nullable (the registry row's mask), and "observable behavior =
// Append(value.ToString())". So the §7 gate publishes a §7.4-narrowed read
// there, `Emitter::lowerExpr` extracts the payload of its `{i8 hasValue; T}`
// carrier, and the rendering must be the payload's own §8.5 `ToString`.
// MEASURED on cb051422 (before the fix-forward): a narrowed user RECORD
// aborted the verifier (`icmp ne i32 %X, i8 0`: the renderer read the bare
// record's first FIELD as the carrier's tag), a narrowed value-like CHOICE
// crashed the compiler (SIGSEGV in emitChoiceToString), and a narrowed
// `Guid?` aborted the same way. The un-narrowed form stays the §7 refusal.
// ===========================================================================

namespace {

std::string declProg(const std::string& members, const std::string& body) {
    return "module M; import System; import System.Text; "
           "record P(X: int, Y: int); "
           "enum Color Red, Green, Blue; end Color; "
           "choice Shape Circle(r: int), Sq(s: int), None; end Shape; "
           "class Dog "
           "  public readonly property Name: string; "
           "  public constructor(n: string) Name := n; end constructor; "
           "end Dog; "
           "class C " + members +
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// §7.4: the declaration's own initializer narrows (probe x14), and a null test
// narrows (w01); two reads of one narrowed binding and an alias (x13).
BEZEL_TEST(builtin_narrowed_ctor_7692, stringbuilder_append_narrowed_record) {
    BEZEL_EXPECT_EQ(runAotOut(declProg("",
        "    var sb := new StringBuilder(); "
        "    var p: P? := new P(1, 2); "
        "    sb.Append(p); "
        "    Console.WriteLine(sb.ToString()); "
        "    var q: P? := new P(3, 4); var r := q; "
        "    if q <> null then sb.Append(q); sb.Append(q); sb.Append(r); end if; "
        "    Console.WriteLine(sb.ToString());")),
        "P(X: 1, Y: 2)\n"
        "P(X: 1, Y: 2)P(X: 3, Y: 4)P(X: 3, Y: 4)P(X: 3, Y: 4)");
}

// §7.4: a parameter guard (`if p = null then return`) narrows the rest of the
// body (probe y05); the null call takes the early return.
BEZEL_TEST(builtin_narrowed_ctor_7692, stringbuilder_append_record_param_guard) {
    BEZEL_EXPECT_EQ(runAotOut(declProg(
        "  public static method G(p: P?): int "
        "    if p = null then return -1; end if; "
        "    var sb := new StringBuilder(); "
        "    sb.Append(p); "
        "    Console.WriteLine(sb.ToString()); "
        "    return sb.Length; "
        "  end G; ",
        "    Console.WriteLine(G(new P(1, 2))); "
        "    Console.WriteLine(G(null));")),
        "P(X: 1, Y: 2)\n13\n-1");
}

// §7.4: a `readonly` field path narrows under a null test (probe y02).
BEZEL_TEST(builtin_narrowed_ctor_7692, stringbuilder_append_record_readonly_field) {
    const std::string src =
        "module M; import System; import System.Text; "
        "record P(X: int, Y: int); "
        "class H "
        "  public readonly _p: P?; "
        "  public constructor(p: P?) _p := p; end constructor; "
        "  public method Go(sb: StringBuilder) "
        "    if _p <> null then sb.Append(_p); end if; "
        "  end Go; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    var sb := new StringBuilder(); "
        "    var h := new H(new P(7, 8)); "
        "    h.Go(sb); "
        "    var n := new H(null); "
        "    n.Go(sb); "
        "    Console.WriteLine(sb.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runAotOut(src), "P(X: 7, Y: 8)");
}

// §7.4: a local initialized from a `?.` read (typed `P?`, §7.6) narrows under
// a null test (probe y03).
BEZEL_TEST(builtin_narrowed_ctor_7692, stringbuilder_append_record_nullcond_local) {
    const std::string src =
        "module M; import System; import System.Text; "
        "record P(X: int, Y: int); "
        "class N "
        "  public readonly property V: P?; "
        "  public constructor(v: P?) V := v; end constructor; "
        "end N; "
        "class C "
        "  public static method Main(): int "
        "    var sb := new StringBuilder(); "
        "    var n: N? := new N(new P(1, 1)); "
        "    var q := n?.V; "
        "    if q <> null then sb.Append(q); end if; "
        "    Console.WriteLine(sb.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runAotOut(src), "P(X: 1, Y: 1)");
}

// A narrowed value-like CHOICE (probe w03; a payload-less case, and the
// `AppendLine` spelling that shares the renderer; `runAotOut` trims the
// trailing line terminator).
BEZEL_TEST(builtin_narrowed_ctor_7692, stringbuilder_append_narrowed_value_choice) {
    BEZEL_EXPECT_EQ(runAotOut(declProg("",
        "    var sb := new StringBuilder(); "
        "    var s: Shape? := Shape.Sq(4); "
        "    if s <> null then sb.Append(s); end if; "
        "    var t: Shape? := Shape.None; "
        "    if t <> null then sb.Append(\";\").Append(t); end if; "
        "    var c: Shape? := Shape.Circle(2); "
        "    sb.Append(\";\"); sb.Append(c); "
        "    if s <> null then sb.AppendLine(s); end if; "
        "    Console.Write(sb.ToString());")),
        "Shape.Sq(s: 4);Shape.None;Shape.Circle(r: 2)Shape.Sq(s: 4)");
}

// The other Named nullables at `Append`: an enum and a class reference
// (controls w02 / w05, green before and after), a `Guid?` and the temporal
// records (`DateOnly?`, `TimeSpan?`, `DateTime?`, x17), and the `!` / `??`
// remedies over a record (x16).
BEZEL_TEST(builtin_narrowed_ctor_7692, stringbuilder_append_narrowed_named_values) {
    BEZEL_EXPECT_EQ(runAotOut(declProg("",
        "    var sb := new StringBuilder(); "
        "    var c: Color? := Color.Green; var d: Dog? := new Dog(\"rex\"); "
        "    var g: Guid? := Guid.Empty; var o: DateOnly? := new DateOnly(2020, 2, 3); "
        "    var t: TimeSpan? := TimeSpan.FromSeconds(5.0); "
        "    var dt: DateTime? := new DateTime(2020, 1, 1); "
        "    if c <> null and d <> null and g <> null and o <> null then "
        "      sb.Append(c); sb.Append(\",\"); sb.Append(d.Name); sb.Append(\",\"); "
        "      sb.Append(g); sb.Append(\",\"); sb.Append(o); "
        "    end if; "
        "    if t <> null and dt <> null then "
        "      sb.Append(\",\"); sb.Append(t); sb.Append(\",\"); sb.Append(dt); "
        "    end if; "
        "    var p: P? := new P(3, 4); "
        "    if p <> null then sb.Append(\",\"); sb.Append(p!); sb.Append(p ?? new P(0, 0)); end if; "
        "    Console.WriteLine(sb.ToString());")),
        "M.Color,rex,00000000-0000-0000-0000-000000000000,2020-02-03,"
        "00:00:05,2020-01-01T00:00:00,P(X: 3, Y: 4)P(X: 3, Y: 4)");
}

// The un-narrowed record is still the §7 refusal at `Append`.
BEZEL_TEST(builtin_narrowed_ctor_7692, stringbuilder_append_unnarrowed_record_refused) {
    auto r = runAot(declProg(
        "  public static method F(x: P?): int "
        "    var sb := new StringBuilder(); sb.Append(x); "
        "    return 0; "
        "  end F; ",
        "    return F(null);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument to `StringBuilder.Append` is nullable but the target "
           "type is non-nullable"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
}

// Many appended narrowed records: each part is a fresh rendered +1 string the
// builder owns and releases (stdlib §3.1, §28; leak repro looped 200x).
BEZEL_TEST(builtin_narrowed_ctor_7692, stringbuilder_append_narrowed_record_loop) {
    BEZEL_EXPECT_EQ(runAotOut(declProg("",
        "    var sb := new StringBuilder(); "
        "    for var i in 0..<200 do "
        "      var p: P? := new P(i, i); "
        "      if p <> null then sb.Append(p); end if; "
        "    end; "
        "    Console.WriteLine(sb.Length);")),
        "3180");
}

// ===========================================================================
// A NAMED argument at the record-taking members (§22.5). Their registry rows
// carry the §7 nullability of the position but no parameter names, so the
// named spelling keeps the loud "named arguments are not supported" refusal
// it had before the rows existed (§22.5, #3398). MEASURED on cb051422, whose
// rows carried names: `dt.CompareTo(other := "x")` (probe x02) and a narrowed
// `dt.Add(span := ts)` (x03) aborted the verifier, and `dt.Add(span := 5)`
// (x01, the named twin of issue #7751) was silently accepted.
// ===========================================================================

namespace {

void expectNamedRefused(const std::string& body) {
    auto r = runAot(prog(body));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "named arguments are not supported on this built-in member"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
}

}  // namespace

BEZEL_TEST(builtin_narrowed_ctor_7692, record_member_named_argument_refused) {
    expectNamedRefused(
        "    var dt := new DateTime(2020, 1, 1); "
        "    Console.WriteLine(dt.CompareTo(other := \"x\"));");
    expectNamedRefused(
        "    var dt := new DateTime(2020, 1, 1); "
        "    var ts: TimeSpan? := TimeSpan.FromSeconds(90.0); "
        "    if ts <> null then Console.WriteLine(dt.Add(span := ts).Minute); end if;");
    expectNamedRefused(
        "    var dt := new DateTime(2020, 1, 1); "
        "    Console.WriteLine(dt.Add(span := 5).Minute);");
    expectNamedRefused(
        "    var dt := new DateTime(2020, 1, 1); "
        "    var o: DateTime? := new DateTime(2021, 1, 1); "
        "    if o <> null then Console.WriteLine(dt.CompareTo(other := o)); end if;");
    expectNamedRefused(
        "    var dd := new DateOnly(2020, 1, 1); "
        "    Console.WriteLine(dd.ToDateTime(time := new TimeOnly(1, 2)).Hour);");
}
