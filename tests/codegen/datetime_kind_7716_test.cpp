// Pin tests for issue #7716: `DateTime.Kind` was unimplemented.
//
// stdlib §2.5 (v0.350) declares
//   public readonly property Kind: DateTimeKind;
//   constructor(year, month, day, hour, minute, second, millisecond := 0,
//               kind := DateTimeKind.Unspecified)
//   static FromTicks(ticks: long, kind: DateTimeKind := DateTimeKind.Unspecified)
// and states, in "Parse and range contracts": `DateTime.Parse` yields
// Kind = Unspecified; `FromUnixTimeSeconds` produces Kind = Utc; `FromTicks`
// raises OverflowException outside MinValue..MaxValue; the CompareTo comment
// says ordering is tick order with Kind NOT normalized. DateOnly.ToDateTime is
// Unspecified. The stdlib does not state Kind for Now / UtcNow / Today /
// ToUtc / ToLocal / the Add* family: the rows for those pin the interpretation
// recorded in the fix commit (Now, Today, ToLocal = Local; UtcNow, ToUtc =
// Utc; arithmetic keeps the receiver's Kind, as .NET does).
//
// Every row reads Kind through the `K` helper (an `=` chain against the three
// enumerators), so the rows pin VALUES, not the enum's rendered text.
//
// Cite: stdlib §2.5, v0.350.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& mainBody) {
    return "module M; "
           "class C "
           "public static method K(d: DateTime): string "
           "if d.Kind = DateTimeKind.Utc then return \"Utc\"; end; "
           "if d.Kind = DateTimeKind.Local then return \"Local\"; end; "
           "return \"Unspecified\"; end K; "
           "public static method Main(): int "
           + mainBody +
           " return 0; end Main; end C;";
}

}  // namespace

// The issue's own repro: the eight-argument constructor, Kind compared with `=`.
BEZEL_TEST(datetime_kind_7716, issue_repro_kind_equals_utc_prints_true) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "var c := new DateTime(2020, 1, 2, 3, 5, 6, 0, DateTimeKind.Utc); "
        "Console.WriteLine(c.Kind = DateTimeKind.Utc); "
        "var d := new DateTime(2024, 3, 4, 5, 6, 7, 8, DateTimeKind.Utc); "
        "Console.WriteLine(d.Kind = DateTimeKind.Utc); "
        "Console.WriteLine(d.Kind = DateTimeKind.Local); ")),
        "True\nTrue\nFalse");
}

// Kind through each constructor spelling.
BEZEL_TEST(datetime_kind_7716, every_constructor_spelling) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "Console.WriteLine(K(new DateTime(2020, 1, 2))); "
        "Console.WriteLine(K(new DateTime(2020, 1, 2, 3, 4, 5))); "
        "Console.WriteLine(K(new DateTime(2020, 1, 2, 3, 4, 5, 6))); "
        "Console.WriteLine(K(new DateTime(2020, 1, 2, 3, 4, 5, 6, DateTimeKind.Unspecified))); "
        "Console.WriteLine(K(new DateTime(2020, 1, 2, 3, 4, 5, 6, DateTimeKind.Utc))); "
        "Console.WriteLine(K(new DateTime(2020, 1, 2, 3, 4, 5, 6, DateTimeKind.Local))); ")),
        "Unspecified\nUnspecified\nUnspecified\nUnspecified\nUtc\nLocal");
}

// A kind argument does not disturb the components the constructor was given.
BEZEL_TEST(datetime_kind_7716, components_are_unaffected_by_kind) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "var l := new DateTime(2021, 12, 31, 23, 59, 58, 7, DateTimeKind.Local); "
        "Console.WriteLine($\"{l.Year}-{l.Month}-{l.Day} {l.Hour}:{l.Minute}:{l.Second}.{l.Millisecond}\"); "
        "Console.WriteLine(l.DayOfYear); "
        "Console.WriteLine(l.Date); "
        "Console.WriteLine(l.TimeOfDay); "
        "Console.WriteLine(l); "
        "Console.WriteLine(l.ToString(\"yyyy-MM-dd\")); "
        "Console.WriteLine(l.Ticks = new DateTime(2021, 12, 31, 23, 59, 58, 7).Ticks); ")),
        "2021-12-31 23:59:58.7\n365\n2021-12-31\n23:59:58\n2021-12-31T23:59:58\n2021-12-31\nTrue");
}

// Every factory and static.
BEZEL_TEST(datetime_kind_7716, factories_and_statics) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "Console.WriteLine(K(DateTime.FromTicks(5L))); "
        "Console.WriteLine(K(DateTime.FromTicks(5L, DateTimeKind.Utc))); "
        "Console.WriteLine(K(DateTime.FromTicks(5L, DateTimeKind.Local))); "
        "Console.WriteLine(DateTime.FromTicks(5L, DateTimeKind.Local).Ticks); "
        "Console.WriteLine(K(DateTime.FromUnixTimeSeconds(5L))); "
        "Console.WriteLine(K(DateTime.Parse(\"2020-01-02T03:04:05\"))); "
        "Console.WriteLine(K(DateTime.Parse(\"2020-01-02\"))); "
        "Console.WriteLine(K(DateTime.UtcNow)); "
        "Console.WriteLine(K(DateTime.Now)); "
        "Console.WriteLine(K(DateTime.Today)); "
        "Console.WriteLine(K(DateTime.MinValue)); "
        "Console.WriteLine(K(DateTime.MaxValue)); "
        "Console.WriteLine(K(new DateOnly(2020, 1, 2).ToDateTime(new TimeOnly(1, 2, 3)))); ")),
        "Unspecified\nUtc\nLocal\n5\nUtc\nUnspecified\nUnspecified\nUtc\nLocal\nLocal"
        "\nUnspecified\nUnspecified\nUnspecified");
}

// ToUtc / ToLocal set the Kind and leave the tick count alone.
BEZEL_TEST(datetime_kind_7716, to_utc_to_local) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "var n := new DateTime(2020, 1, 2, 3, 4, 5); "
        "Console.WriteLine(K(n.ToUtc())); "
        "Console.WriteLine(K(n.ToLocal())); "
        "Console.WriteLine(K(n.ToLocal().ToUtc())); "
        "Console.WriteLine(n.ToUtc().Ticks = n.Ticks); "
        "Console.WriteLine(n.ToLocal().ToString()); ")),
        "Utc\nLocal\nUtc\nTrue\n2020-01-02T03:04:05");
}

// Arithmetic keeps the receiver's Kind: the whole Add* family, Add, Subtract(span)
// and the operators.
BEZEL_TEST(datetime_kind_7716, arithmetic_preserves_kind) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "var u := new DateTime(2020, 1, 31, 3, 4, 5, 0, DateTimeKind.Utc); "
        "var l := new DateTime(2020, 1, 31, 3, 4, 5, 0, DateTimeKind.Local); "
        "var s := TimeSpan.FromHours(30.0); "
        "Console.WriteLine(K(u.AddYears(1)) + K(l.AddYears(1))); "
        "Console.WriteLine(K(u.AddMonths(1)) + K(l.AddMonths(1))); "
        "Console.WriteLine(K(u.AddDays(1.5)) + K(l.AddDays(1.5))); "
        "Console.WriteLine(K(u.AddHours(2.0)) + K(l.AddHours(2.0))); "
        "Console.WriteLine(K(u.AddMinutes(2.0)) + K(l.AddMinutes(2.0))); "
        "Console.WriteLine(K(u.AddSeconds(2.0)) + K(l.AddSeconds(2.0))); "
        "Console.WriteLine(K(u.AddMilliseconds(2.0)) + K(l.AddMilliseconds(2.0))); "
        "Console.WriteLine(K(u.Add(s)) + K(l.Add(s))); "
        "Console.WriteLine(K(u.Subtract(s)) + K(l.Subtract(s))); "
        "Console.WriteLine(K(u + s) + K(l + s)); "
        "Console.WriteLine(K(s + u) + K(s + l)); "
        "Console.WriteLine(K(u - s) + K(l - s)); "
        "Console.WriteLine(u.AddMonths(1)); "
        "Console.WriteLine(l.AddYears(1)); "
        "Console.WriteLine(u.AddDays(1.5)); "
        "Console.WriteLine(l + s); "
        "Console.WriteLine(s + u); "
        "Console.WriteLine(u - s); ")),
        "UtcLocal\nUtcLocal\nUtcLocal\nUtcLocal\nUtcLocal\nUtcLocal\nUtcLocal\nUtcLocal\nUtcLocal"
        "\nUtcLocal\nUtcLocal\nUtcLocal\n"
        "2020-02-29T03:04:05\n2021-01-31T03:04:05\n2020-02-01T15:04:05\n"
        "2020-02-01T09:04:05\n2020-02-01T09:04:05\n2020-01-29T21:04:05");
}

// A difference of two DateTimes is a TimeSpan over the tick counts: neither
// Kind takes part, and a DateTime-typed result is not produced.
BEZEL_TEST(datetime_kind_7716, difference_ignores_kind) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "var u := new DateTime(2020, 1, 2, 3, 4, 5, 0, DateTimeKind.Utc); "
        "var l := new DateTime(2020, 1, 1, 3, 4, 5, 0, DateTimeKind.Local); "
        "var n := new DateTime(2020, 1, 1, 3, 4, 5); "
        "Console.WriteLine(u - l); "
        "Console.WriteLine(l - u); "
        "Console.WriteLine(u - n); "
        "Console.WriteLine(u.Subtract(l)); "
        "Console.WriteLine(l.Subtract(n)); ")),
        "1.00:00:00\n-1.00:00:00\n1.00:00:00\n1.00:00:00\n00:00:00");
}

// Equality is TICK equality: the Kind does not take part. stdlib §2.5 states
// "CompareTo = tick order (Kind not normalized, matching .NET)" and is silent
// on `=`; the interim coordinator decision recorded on spec-question #7764 keeps
// the pre-#7716 tick equality (an equality that disagreed with CompareTo made
// `DateTime.Today = new DateTime(t.Year, t.Month, t.Day)` False). So two values
// equal in ticks but different in Kind are `=` and compare 0, while each keeps
// its own Kind; a Local value orders by ticks like any other.
BEZEL_TEST(datetime_kind_7716, equality_and_compareto) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "var u := new DateTime(2020, 1, 2, 3, 4, 5, 0, DateTimeKind.Utc); "
        "var u2 := new DateTime(2020, 1, 2, 3, 4, 5, 0, DateTimeKind.Utc); "
        "var l := new DateTime(2020, 1, 2, 3, 4, 5, 0, DateTimeKind.Local); "
        "var lLater := new DateTime(2020, 1, 2, 3, 4, 6, 0, DateTimeKind.Local); "
        "var n := new DateTime(2020, 1, 2, 3, 4, 5); "
        "Console.WriteLine(u = u2); "
        "Console.WriteLine(u = l); "
        "Console.WriteLine(u = n); "
        "Console.WriteLine(u <> l); "
        "Console.WriteLine(l <> lLater); "
        "Console.WriteLine(K(u) + K(l) + K(n)); "
        "Console.WriteLine(u.CompareTo(l)); "
        "Console.WriteLine(l.CompareTo(u)); "
        "Console.WriteLine(u.CompareTo(lLater)); "
        "Console.WriteLine(lLater.CompareTo(u)); "
        "Console.WriteLine(lLater.CompareTo(n)); "
        "Console.WriteLine(n.CompareTo(lLater)); ")),
        "True\nTrue\nTrue\nFalse\nTrue\nUtcLocalUnspecified\n0\n0\n-1\n1\n1\n-1");
}

// Kind survives a round trip through a record field and a nullable; the
// record's synthesized equality compares the DateTime field by ticks (#7764's
// interim decision, as `equality_and_compareto` above), so Kind is read with
// `.Kind`, never through `=` on the record.
BEZEL_TEST(datetime_kind_7716, kind_survives_storage) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "record Stamp(At: DateTime, N: int); "
        "class C "
        "public static method Main(): int "
        "var s := new Stamp(new DateTime(2020, 1, 2, 3, 4, 5, 0, DateTimeKind.Local), 1); "
        "var t := new Stamp(new DateTime(2020, 1, 2, 3, 4, 5, 0, DateTimeKind.Local), 1); "
        "var u := new Stamp(new DateTime(2020, 1, 2, 3, 4, 5, 0, DateTimeKind.Utc), 1); "
        "Console.WriteLine(s.At.Kind = DateTimeKind.Local); "
        "Console.WriteLine(u.At.Kind = DateTimeKind.Utc); "
        "Console.WriteLine(s = t); "
        "Console.WriteLine(s = u); "
        "var nl: DateTime? := s.At; "
        "var nu: DateTime? := u.At; "
        "Console.WriteLine(nl!.Kind = DateTimeKind.Local); "
        "Console.WriteLine(nu!.Kind = DateTimeKind.Utc); "
        "Console.WriteLine(nl = nu); "
        "return 0; end Main; end C;"),
        "True\nTrue\nTrue\nTrue\nTrue\nTrue\nTrue");
}

// stdlib §2.5: FromTicks raises OverflowException outside MinValue..MaxValue.
// The Kind lives in the top two bits of the scalar, so the bound is also what
// keeps a tick count from being read back as a different Kind.
BEZEL_TEST(datetime_kind_7716, fromticks_out_of_range_raises_overflow) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "try var d := DateTime.FromTicks(3155378975999999999L, DateTimeKind.Utc); "
        "Console.WriteLine(K(d)); Console.WriteLine(d = DateTime.MaxValue.ToUtc()); "
        "catch OverflowException => Console.WriteLine(\"OVF\"); end; "
        "try var d := DateTime.FromTicks(3155378976000000000L); "
        "Console.WriteLine($\"no raise {d.Ticks}\"); "
        "catch OverflowException => Console.WriteLine(\"OVF\"); end; "
        "try var d := DateTime.FromTicks(-1L, DateTimeKind.Utc); "
        "Console.WriteLine($\"no raise {d.Ticks}\"); "
        "catch OverflowException => Console.WriteLine(\"OVF\"); end; ")),
        "Utc\nTrue\nOVF\nOVF");
}

// Review of the first #7716 cut (probe p08): SortedSet<DateTime> and
// SortedDictionary<DateTime, V> shared the `long` comparator, which compared
// the raw word -- every Local value (bit 63 set) sorted first and every Utc
// value after every Unspecified one, and a set kept two CompareTo-equal values.
// stdlib §4.18: "adding an equal element leaves the set unmodified", equality
// being CompareTo = 0; stdlib §2.5: CompareTo is tick order. The duplicate
// that is kept is the first one added, Kind and all.
BEZEL_TEST(datetime_kind_7716, sorted_containers_order_mixed_kinds_by_ticks) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "var s := new SortedSet<DateTime>(); "
        "s.Add(new DateTime(2100, 1, 1, 0, 0, 0, 0, DateTimeKind.Local)); "
        "s.Add(new DateTime(2000, 1, 1, 0, 0, 0, 0, DateTimeKind.Utc)); "
        "s.Add(new DateTime(2050, 1, 1, 0, 0, 0, 0, DateTimeKind.Local)); "
        "s.Add(new DateTime(2001, 1, 1)); "
        "s.Add(new DateTime(2050, 1, 1, 0, 0, 0, 0, DateTimeKind.Utc)); "
        "s.Add(new DateTime(2001, 1, 1, 0, 0, 0, 0, DateTimeKind.Local)); "
        "Console.WriteLine(s.Count); "
        "for var x in s do Console.WriteLine($\"{x.Year} {K(x)}\"); end; "
        "Console.WriteLine(s.First.Year); "
        "Console.WriteLine(s.Last.Year); "
        "Console.WriteLine(s.Contains(new DateTime(2000, 1, 1))); "
        "var sd := new SortedDictionary<DateTime, int>(); "
        "sd[new DateTime(2100, 1, 1, 0, 0, 0, 0, DateTimeKind.Local)] := 1; "
        "sd[new DateTime(2000, 1, 1, 0, 0, 0, 0, DateTimeKind.Utc)] := 2; "
        "sd[new DateTime(2001, 1, 1)] := 3; "
        "sd[new DateTime(2000, 1, 1, 0, 0, 0, 0, DateTimeKind.Local)] := 4; "
        "Console.WriteLine(sd.Count); "
        "for var e in sd do Console.WriteLine($\"{e.Key.Year} {e.Value}\"); end; "
        "Console.WriteLine(sd.FirstKey.Year); "
        "Console.WriteLine(sd.LastKey.Year); ")),
        "4\n2000 Utc\n2001 Unspecified\n2050 Local\n2100 Local\n2000\n2100\nTrue\n"
        "3\n2000 4\n2001 3\n2100 1\n2000\n2100");
}

// Review probe p11: Now and Today are Local and UtcNow is Utc, so the raw-word
// comparator put Today/Now ahead of a 2000 date and UtcNow behind a 2100
// Unspecified one. Tick order puts the 2000 value first and the 2100 value
// last, with the three clock readings in between in CompareTo order. The
// clock values themselves are not printed (they change).
BEZEL_TEST(datetime_kind_7716, sorted_containers_order_now_utcnow_today_by_ticks) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "var s := new SortedSet<DateTime>(); "
        "s.Add(DateTime.Today); "
        "s.Add(DateTime.Now); "
        "s.Add(new DateTime(2100, 1, 1)); "
        "s.Add(DateTime.UtcNow); "
        "s.Add(new DateTime(2000, 1, 1)); "
        "Console.WriteLine(s.First.Year); "
        "Console.WriteLine(s.Last.Year); "
        "var prev := DateTime.MinValue; var ordered := true; "
        "for var x in s do if prev.CompareTo(x) >= 0 then ordered := false; end; prev := x; end; "
        "Console.WriteLine(ordered); "
        "var sd := new SortedDictionary<DateTime, int>(); "
        "sd[DateTime.Now] := 1; "
        "sd[new DateTime(2100, 1, 1)] := 2; "
        "sd[DateTime.UtcNow] := 3; "
        "sd[new DateTime(2000, 1, 1)] := 4; "
        "sd[DateTime.Today] := 5; "
        "Console.WriteLine(sd.FirstKey.Year); "
        "Console.WriteLine(sd.LastKey.Year); "
        "var firstVal := 0; "
        "for var e in sd do firstVal := e.Value; break; end; "
        "Console.WriteLine(firstVal); ")),
        "2000\n2100\nTrue\n2000\n2100\n4");
}

// Review probe p23: `DateTime.Today` is Local and the three-argument
// constructor is Unspecified, so a Kind-honouring equality made the two
// unequal and every hash lookup miss -- base (pre-#7716) answered True. Tick
// equality (#7764's interim decision) restores it for `=` and for every
// lookup that uses the §8.3 pair.
BEZEL_TEST(datetime_kind_7716, today_equals_the_constructed_date) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "var t := DateTime.Today; "
        "var built := new DateTime(t.Year, t.Month, t.Day); "
        "Console.WriteLine(K(t) + K(built)); "
        "Console.WriteLine(t = built); "
        "Console.WriteLine(t <> built); "
        "Console.WriteLine(t.CompareTo(built)); "
        "var m := new Dictionary<DateTime, string>(); "
        "m[built] := \"x\"; "
        "Console.WriteLine(m.ContainsKey(t)); "
        "var hs := new Set<DateTime>(); "
        "hs.Add(built); "
        "Console.WriteLine(hs.Contains(t)); "
        "var a: Array<DateTime> := [built]; "
        "Console.WriteLine(a.Contains(t)); "
        "Console.WriteLine(a.IndexOf(t)); ")),
        "LocalUnspecified\nTrue\nFalse\n0\nTrue\nTrue\nTrue\n0");
}

// Tick equality holds across every §8.3 consumer, and each one hashes
// consistently with it: Dictionary and Set dedup, a record key with a DateTime
// field, `DateTime?` equality, LINQ Distinct / ToSet / GroupBy, and Array
// Remove. A dictionary entry re-assigned through a CompareTo-equal key of
// another Kind updates the one entry.
BEZEL_TEST(datetime_kind_7716, tick_equality_across_hash_consumers) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "record Stamp(At: DateTime, N: int); "
        "class C "
        "public static method Main(): int "
        "var u := new DateTime(2020, 1, 2, 3, 4, 5, 0, DateTimeKind.Utc); "
        "var l := new DateTime(2020, 1, 2, 3, 4, 5, 0, DateTimeKind.Local); "
        "var n := new DateTime(2020, 1, 2, 3, 4, 5); "
        "var later := new DateTime(2021, 1, 1, 0, 0, 0, 0, DateTimeKind.Local); "
        "var d := new Dictionary<DateTime, int>(); "
        "d[u] := 1; d[l] := 2; d[n] := 3; d[later] := 4; "
        "Console.WriteLine(d.Count); "
        "Console.WriteLine(d[u]); "
        "var hs := new Set<DateTime>(); "
        "hs.Add(u); hs.Add(l); hs.Add(n); hs.Add(later); "
        "Console.WriteLine(hs.Count); "
        "var ds := new Dictionary<Stamp, int>(); "
        "ds[new Stamp(u, 1)] := 1; ds[new Stamp(l, 1)] := 2; ds[new Stamp(l, 2)] := 3; "
        "Console.WriteLine(ds.Count); "
        "var nu: DateTime? := u; "
        "var nl: DateTime? := l; "
        "Console.WriteLine(nu = nl); "
        "Console.WriteLine(nu = l); "
        "var a: Array<DateTime> := [u, l, n, later]; "
        "Console.WriteLine(a.Distinct().ToArray().Count); "
        "Console.WriteLine(a.ToSet().Count); "
        "Console.WriteLine(a.GroupBy(lambda(x) => x).ToArray().Count); "
        "Console.WriteLine(a.Remove(n)); "
        "Console.WriteLine(a.Count); "
        "return 0; end Main; end C;"),
        "2\n3\n2\n2\nTrue\nTrue\n2\n2\n2\nTrue\n3");
}

namespace {

// `P(d)` renders a DateTime as "<day> <Kind>", so a row pins both WHICH value
// the call returned (its date) and that the winner kept its OWN Kind. Every
// operand is in January 2020; L / C / U name Local / Utc / Unspecified and the
// digit is the day.
std::string mathProg(const std::string& mainBody) {
    return "module M; "
           "class G "
           "public static method Lo<T>(x: T, y: T): T where T: IComparable<T> "
           "return Math.Min(x, y); end Lo; "
           "public static method Hi<T>(x: T, y: T): T where T: IComparable<T> "
           "return Math.Max(x, y); end Hi; "
           "public static method Cl<T>(v: T, lo: T, hi: T): T where T: IComparable<T> "
           "return Math.Clamp(v, lo, hi); end Cl; "
           "end G; "
           "class C "
           "public static method K(d: DateTime): string "
           "if d.Kind = DateTimeKind.Utc then return \"Utc\"; end; "
           "if d.Kind = DateTimeKind.Local then return \"Local\"; end; "
           "return \"Unspecified\"; end K; "
           "public static method P(d: DateTime): string "
           "return $\"{d.Day} {K(d)}\"; end P; "
           "public static method Main(): int "
           "var l1 := new DateTime(2020, 1, 1, 0, 0, 0, 0, DateTimeKind.Local); "
           "var c2 := new DateTime(2020, 1, 2, 0, 0, 0, 0, DateTimeKind.Utc); "
           "var u3 := new DateTime(2020, 1, 3); "
           "var l4 := new DateTime(2020, 1, 4, 0, 0, 0, 0, DateTimeKind.Local); "
           "var c5 := new DateTime(2020, 1, 5, 0, 0, 0, 0, DateTimeKind.Utc); "
           "var u6 := new DateTime(2020, 1, 6); "
           + mainBody +
           " return 0; end Main; end C;";
}

}  // namespace

// stdlib §2.2 (v0.350): Math.Min / Math.Max order by T's CompareTo, and for
// DateTime that is tick order with the Kind NOT normalized (stdlib §2.5). The
// result is one of the arguments, so it keeps that argument's Kind. Every pair
// of Kinds, each with the earlier date first and then second, in both
// argument orders. The raw 64-bit word put a Local value (bit 63 set) below
// every other and a Utc one (bit 62) above every Unspecified one: before the
// fix `Math.Max(l4, u3)` returned u3.
BEZEL_TEST(datetime_kind_7716, math_min_max_order_mixed_kinds_by_ticks) {
    BEZEL_EXPECT_EQ(runAotOut(mathProg(
        // Local vs Unspecified
        "Console.WriteLine(P(Math.Min(l1, u3)) + \" | \" + P(Math.Max(l1, u3))); "
        "Console.WriteLine(P(Math.Min(u3, l1)) + \" | \" + P(Math.Max(u3, l1))); "
        "Console.WriteLine(P(Math.Min(l4, u3)) + \" | \" + P(Math.Max(l4, u3))); "
        "Console.WriteLine(P(Math.Min(u3, l4)) + \" | \" + P(Math.Max(u3, l4))); "
        // Utc vs Unspecified
        "Console.WriteLine(P(Math.Min(c2, u3)) + \" | \" + P(Math.Max(c2, u3))); "
        "Console.WriteLine(P(Math.Min(u3, c2)) + \" | \" + P(Math.Max(u3, c2))); "
        "Console.WriteLine(P(Math.Min(c5, u3)) + \" | \" + P(Math.Max(c5, u3))); "
        "Console.WriteLine(P(Math.Min(u3, c5)) + \" | \" + P(Math.Max(u3, c5))); "
        // Local vs Utc
        "Console.WriteLine(P(Math.Min(l1, c2)) + \" | \" + P(Math.Max(l1, c2))); "
        "Console.WriteLine(P(Math.Min(c2, l1)) + \" | \" + P(Math.Max(c2, l1))); "
        "Console.WriteLine(P(Math.Min(l4, c2)) + \" | \" + P(Math.Max(l4, c2))); "
        "Console.WriteLine(P(Math.Min(c2, l4)) + \" | \" + P(Math.Max(c2, l4))); ")),
        "1 Local | 3 Unspecified\n"
        "1 Local | 3 Unspecified\n"
        "3 Unspecified | 4 Local\n"
        "3 Unspecified | 4 Local\n"
        "2 Utc | 3 Unspecified\n"
        "2 Utc | 3 Unspecified\n"
        "3 Unspecified | 5 Utc\n"
        "3 Unspecified | 5 Utc\n"
        "1 Local | 2 Utc\n"
        "1 Local | 2 Utc\n"
        "2 Utc | 4 Local\n"
        "2 Utc | 4 Local");
}

// stdlib §2.2 (v0.350): "a tie returns `a` in both" Min and Max. Two
// DateTimes with equal ticks and different Kinds are CompareTo-equal
// (stdlib §2.5) yet distinguishable, so the tie rule decides the Kind.
BEZEL_TEST(datetime_kind_7716, math_min_max_tick_tie_returns_first_argument) {
    BEZEL_EXPECT_EQ(runAotOut(mathProg(
        "var cu := new DateTime(2020, 1, 3, 0, 0, 0, 0, DateTimeKind.Utc); "
        "var cl := new DateTime(2020, 1, 3, 0, 0, 0, 0, DateTimeKind.Local); "
        "Console.WriteLine(P(Math.Min(cu, u3)) + \" | \" + P(Math.Max(cu, u3))); "
        "Console.WriteLine(P(Math.Min(u3, cu)) + \" | \" + P(Math.Max(u3, cu))); "
        "Console.WriteLine(P(Math.Min(cl, cu)) + \" | \" + P(Math.Max(cl, cu))); "
        "Console.WriteLine(P(Math.Min(cu, cl)) + \" | \" + P(Math.Max(cu, cl))); ")),
        "3 Utc | 3 Utc\n"
        "3 Unspecified | 3 Unspecified\n"
        "3 Local | 3 Local\n"
        "3 Utc | 3 Utc");
}

// stdlib §2.2 (v0.350): Clamp(value, low, high) returns `low` when value
// compares less than low, `high` when it compares greater than high, else
// `value`, by DateTime's tick CompareTo. Each row is a mix of Kinds that the
// raw-word compare got wrong (below, inside and above the range, and the
// value's own Kind as each of the three), and the winner keeps its Kind.
BEZEL_TEST(datetime_kind_7716, math_clamp_orders_mixed_kinds_by_ticks) {
    BEZEL_EXPECT_EQ(runAotOut(mathProg(
        "Console.WriteLine(P(Math.Clamp(c5, l1, u3))); "   // above: high
        "Console.WriteLine(P(Math.Clamp(l4, l1, u3))); "   // above: high
        "Console.WriteLine(P(Math.Clamp(u6, c2, l4))); "   // above: high
        "Console.WriteLine(P(Math.Clamp(l1, c2, u6))); "   // below: low
        "Console.WriteLine(P(Math.Clamp(u3, l4, c5))); "   // below: low
        "Console.WriteLine(P(Math.Clamp(c2, u3, l4))); "   // below: low
        "Console.WriteLine(P(Math.Clamp(l4, c2, u6))); "   // inside: value
        "Console.WriteLine(P(Math.Clamp(c2, l1, u3))); "   // inside: value
        "Console.WriteLine(P(Math.Clamp(u3, c2, l4))); ")),  // inside: value
        "3 Unspecified\n"
        "3 Unspecified\n"
        "4 Local\n"
        "2 Utc\n"
        "4 Local\n"
        "3 Unspecified\n"
        "4 Local\n"
        "2 Utc\n"
        "3 Unspecified");
}

// The same ordering at a `T = DateTime` substitution of a generic body (the
// operand is typed `T` there) and through an explicit `Math.Max<DateTime>`.
BEZEL_TEST(datetime_kind_7716, math_min_max_clamp_generic_and_explicit_t) {
    BEZEL_EXPECT_EQ(runAotOut(mathProg(
        "Console.WriteLine(P(G.Lo(l4, u3)) + \" | \" + P(G.Hi(l4, u3))); "
        "Console.WriteLine(P(G.Lo(u3, c2)) + \" | \" + P(G.Hi(u3, c2))); "
        "Console.WriteLine(P(G.Cl(l1, c2, u3)) + \" | \" + P(G.Cl(c5, u3, l4))); "
        "Console.WriteLine(P(Math.Max<DateTime>(l4, c2)) + \" | \" + P(Math.Min<DateTime>(c5, l4))); "
        "Console.WriteLine(P(Math.Clamp<DateTime>(u6, l1, c5))); ")),
        "3 Unspecified | 4 Local\n"
        "2 Utc | 3 Unspecified\n"
        "2 Utc | 4 Local\n"
        "4 Local | 4 Local\n"
        "5 Utc");
}
