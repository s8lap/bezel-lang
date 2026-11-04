// Regression tests for #7756, #7681, #7710 and the named-extension half of
// #7936 -- a user extension on a built-in head was never dispatched when the
// receiver's type came from a STATIC FACTORY or a BUILT-IN CALL RESULT
// (`Guid.Empty`, `TimeSpan.FromHours(1.0)`, `d.AddDays(1)`,
// `FrozenArray<int>.From(a)`, `DateTime.FromTicks(5L)`), while the same
// call on an ANNOTATED local (`var t: TimeSpan := ...`) worked.
//
// WHAT THE SPEC REQUIRES.  Language spec §26.5 rule 2 (v0.355): "If the
// receiver's type declares no instance member of that name, the compiler
// searches ... for extension blocks on `x`'s type, base types, or
// implemented interfaces"; §26.2 lists the built-in primitive types and
// classes as extendable, and §26.1 admits methods and computed read-only
// properties.  The receiver's TYPE is the same whichever expression shape
// produced it: an inferred `var t := TimeSpan.FromHours(1.0)` has type
// `TimeSpan` exactly as `var t: TimeSpan := ...` does.
//
// THE DEFECT.  The two receiver shapes carry different TypeRefs: an
// annotation resolves to a declaration-less Named `TimeSpan`, while a
// factory / built-in call result is stamped with the synthetic
// `builtinClassSym` TypeSymbol (non-null declaration, no TypeDecl, no
// module).  The §26.5 matcher `extensionTargetMatchesReceiver` classified a
// TypeRef as built-in only when its declaration was null, so it read the
// stub as a USER type and refused to equate it with the extension's
// declaration-less head: the extension was invisible.  Codegen then
// reported "produced no value" (a unique name), silently dropped the
// statement (an extension property), or kept the built-in (`Sum` on
// FrozenArray).  The #7946 codegen decline had the same blind spot
// (`!recvTy->declaration`), so the built-in name-keyed arms kept answering
// for a stub-typed concrete collection receiver.
//
// ORACLE: exact stdout (runAotOut / runAot().stdoutText); "<did-not-run>"
// is the pre-fix answer for every rejected row.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {
std::string prog(const std::string& ext, const std::string& body) {
    return "module M; " + ext +
           " class C public static method Main(): int " + body +
           " return 0; end Main; end C;";
}
std::string out(const std::string& src) {
    auto r = runAot(src);
    if (!r.ran) return "<did-not-run>";
    return r.stdoutText;
}
}  // namespace

// ---- #7756: TimeSpan / Guid factory receivers ------------------------------

// §26.5 rule 2 (v0.355): `Guid` declares no `Short`; the receiver is the
// static property `Guid.Empty`.  The issue's own repro.
BEZEL_TEST(extension_builtin_stub_receiver_7756, guid_empty_static_property) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("extension Guid public method Short(): string => "
                       "this.ToString(\"N\"); end;",
                       "Console.WriteLine(Guid.Empty.Short());")),
        "00000000000000000000000000000000");
}

// §26.5 rule 2 / §26.1 (v0.355): a method AND a computed property on an
// inferred local initialised from the `FromHours` factory.
BEZEL_TEST(extension_builtin_stub_receiver_7756, timespan_factory_local) {
    BEZEL_EXPECT_EQ(
        out(prog("extension TimeSpan public method Twice(): TimeSpan => "
                 "TimeSpan.FromTicks(this.Ticks * 2L); public property Mins: "
                 "double => this.TotalMinutes; end;",
                 "var t := TimeSpan.FromHours(1.0); "
                 "Console.WriteLine(t.Twice()); Console.WriteLine(t.Mins);")),
        "02:00:00\n60\n");
}

// §26.5 rule 2 (v0.355): stdlib §2.5 declares no instance `Add` on
// `TimeSpan` (only `operator +`), so a user `Add` is selected.
BEZEL_TEST(extension_builtin_stub_receiver_7756, timespan_factory_add_name) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("extension TimeSpan public method Add(o: TimeSpan): "
                       "TimeSpan => TimeSpan.FromTicks(this.Ticks + o.Ticks); "
                       "end;",
                       "var t := TimeSpan.FromHours(1.0); "
                       "Console.WriteLine(t.Add(t));")),
        "02:00:00");
}

// §26.5 rule 2 (v0.355): the factory result as a DIRECT receiver (no local).
BEZEL_TEST(extension_builtin_stub_receiver_7756, timespan_factory_chained) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("extension TimeSpan public method Half(): TimeSpan => "
                       "TimeSpan.FromTicks(this.Ticks / 2L); end;",
                       "Console.WriteLine(TimeSpan.FromTicks(10L).Half()"
                       ".Ticks);")),
        "5");
}

// §26.5 rule 2 (v0.355): #7607's repro -- the same receiver shape inside an
// interpolated string, where the undispatched call was SILENTLY rendered as
// 0 (#1386 placeholder warnings, exit 0) instead of being rejected.
// Stdlib §2.5 declares no instance `Add` / `Negate` / `Subtract` on
// `TimeSpan`, so each user method is selected.
BEZEL_TEST(extension_builtin_stub_receiver_7756, timespan_factory_interpolated_7607) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("extension TimeSpan public method Add(other: TimeSpan): "
                       "TimeSpan => TimeSpan.FromTicks(this.Ticks + "
                       "other.Ticks); public method Negate(): TimeSpan => "
                       "TimeSpan.FromTicks(0L - this.Ticks); public method "
                       "Subtract(other: TimeSpan): TimeSpan => "
                       "TimeSpan.FromTicks(this.Ticks - other.Ticks); end;",
                       "var a := TimeSpan.FromTicks(5L); "
                       "var b := TimeSpan.FromTicks(7L); "
                       "Console.WriteLine($\"{a.Add(b).Ticks} "
                       "{a.Negate().Ticks} {a.Subtract(b).Ticks}\");")),
        "12 -5 -2");
}

// ---- #7681: DateTime built-in CALL RESULT receivers ------------------------

// §26.5 rule 2 (v0.355): the issue's p13 -- an extension method on
// `d.AddDays(1)`, and a chain of extension calls over call results.
BEZEL_TEST(extension_builtin_stub_receiver_7756, datetime_call_result_method_7681) {
    BEZEL_EXPECT_EQ(
        out(prog("extension DateTime public method IsLeap(): bool => "
                 "(this.Year mod 4) = 0; public method Plus(n: int): DateTime "
                 "=> this.AddDays(n); public method YearPlus(n: int): int => "
                 "this.Year + n; end;",
                 "var d := new DateTime(2024, 3, 15, 10, 20, 30, 456); "
                 "Console.WriteLine(d.AddDays(1).IsLeap()); "
                 "Console.WriteLine(d.Plus(1).Plus(2).Year); "
                 "Console.WriteLine(d.AddDays(1).Plus(2).YearPlus(1));")),
        "True\n2024\n2025\n");
}

// §26.1 / §26.5 rule 2 (v0.355): the issue's p21b -- an extension PROPERTY
// on a call result was SILENTLY DROPPED (the statement emitted nothing and
// the program exited 0), so the oracle is the full stdout, all four lines.
BEZEL_TEST(extension_builtin_stub_receiver_7756, datetime_call_result_property_7681) {
    BEZEL_EXPECT_EQ(
        out(prog("extension DateTime public property LateMonth: bool => "
                 "this.Day > 15; public property Quarter: int => "
                 "(this.Month - 1) / 3 + 1; end;",
                 "var d := new DateTime(2024, 3, 16); "
                 "Console.WriteLine(d.LateMonth); "
                 "Console.WriteLine(d.Quarter); "
                 "Console.WriteLine(d.AddDays(1).LateMonth); "
                 "Console.WriteLine(d.AddMonths(1).Quarter);")),
        "True\n1\nTrue\n2\n");
}

// §26.5 rule 2 (v0.355): an inferred LOCAL from a built-in call result.
BEZEL_TEST(extension_builtin_stub_receiver_7756, datetime_call_result_local_7681) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("extension DateTime public method IsLeap(): bool => "
                       "(this.Year mod 4) = 0; end;",
                       "var d := new DateTime(2024, 3, 15); "
                       "var e := d.AddDays(1); "
                       "Console.WriteLine(e.IsLeap());")),
        "True");
}

// ---- #7936 (named-extension half): DateTime factory receivers -------------

// §26.5 rule 2 (v0.355): stdlib §2.5 declares no `AddTicks` on `DateTime`.
// The issue's t_ext_factory repro.
BEZEL_TEST(extension_builtin_stub_receiver_7756, datetime_fromticks_local_7936) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("extension DateTime public method AddTicks(n: long): "
                       "long => this.Ticks + n; end;",
                       "var d := DateTime.FromTicks(5L); "
                       "Console.WriteLine(d.AddTicks(5L));")),
        "10");
}

// §26.5 rule 2 (v0.355): the same, chained directly on the factory.
BEZEL_TEST(extension_builtin_stub_receiver_7756, datetime_fromticks_chained_7936) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("extension DateTime public method AddTicks(n: long): "
                       "long => this.Ticks + n; end;",
                       "Console.WriteLine(DateTime.FromTicks(5L)"
                       ".AddTicks(5L));")),
        "10");
}

// ---- #7710: FrozenArray<T>.From(...) receivers -----------------------------

// §26.5 rules 2-3 / §26.5.1 (v0.355): `FrozenArray<T>` (stdlib §4.19)
// declares no `Sum`; the user's `extension FrozenArray<int>` block is more
// specific than the stdlib `extension IEnumerable<T>` block, so its 77 is
// the answer -- pre-fix the built-in's 6 was printed.
BEZEL_TEST(extension_builtin_stub_receiver_7756, frozenarray_from_sum_7710) {
    BEZEL_EXPECT_EQ(
        runAotOut("module R; extension FrozenArray<int> public method Sum(): "
                  "int => 77; end; class Program public static method Main(): "
                  "int var a: Array<int> := [1,2,3]; var f := "
                  "FrozenArray<int>.From(a); Console.WriteLine(f.Sum()); "
                  "return 0; end Main; end Program;"),
        "77");
}

// §26.5 rules 2-3 (v0.355): `using var` local, `Max` and `Sum` both
// user-supplied -- pre-fix the built-ins' 3 and 6 were printed.
BEZEL_TEST(extension_builtin_stub_receiver_7756, frozenarray_from_max_sum_using_7710) {
    BEZEL_EXPECT_EQ(
        out("module R; extension FrozenArray<int> public method Max(): int "
            "=> 77; public method Sum(): int => 78; end; class Program public "
            "static method Main(): int var a: Array<int> := [1,2,3]; using var "
            "f := FrozenArray<int>.From(a); Console.WriteLine(f.Max()); "
            "Console.WriteLine(f.Sum()); return 0; end Main; end Program;"),
        "77\n78\n");
}

// §26.5 rule 2 (v0.355): names `FrozenArray<T>` does not declare (`Foo`,
// and the §5 operators `Any` / `First`) -- pre-fix "produced no value".
BEZEL_TEST(extension_builtin_stub_receiver_7756, frozenarray_from_unique_names_7710) {
    BEZEL_EXPECT_EQ(
        out("module R; extension FrozenArray<int> public method Foo(): int "
            "=> 77; public method Any(): int => 78; public method First(): "
            "int => 79; end; class Program public static method Main(): int "
            "var a: Array<int> := [1,2,3]; using var f := "
            "FrozenArray<int>.From(a); Console.WriteLine(f.Foo()); "
            "Console.WriteLine(f.Any()); Console.WriteLine(f.First()); "
            "return 0; end Main; end Program;"),
        "77\n78\n79\n");
}

// §26.5 rules 1-2 / §22.6 (v0.355): stdlib §4.19 declares no `Peek` or
// `Push` on `FrozenArray<T>`, so rule 1 does not commit and the extension is
// selected.  This row pins the CODEGEN half: once sema bound the call to the
// extension, the name-keyed built-in arms still claimed a stub-typed
// receiver because the #7946 decline gated on `!recvTy->declaration` --
// with only the sema half fixed the program printed 3 (the built-in Peek)
// and 0.  A factory call as the DIRECT receiver must also be evaluated once
// (§26.7): "mk" appears once per call.
BEZEL_TEST(extension_builtin_stub_receiver_7756, frozenarray_from_name_keyed_arms_decline_7710) {
    BEZEL_EXPECT_EQ(
        out("module R; extension FrozenArray<int> public method Peek(): int "
            "=> 7; public method Push(n: int): int => 8; end; class Mk "
            "public static method F(a: Array<int>): FrozenArray<int> "
            "Console.WriteLine(\"mk\"); return FrozenArray<int>.From(a); end "
            "F; end Mk; class Program public static method Main(): int var "
            "a: Array<int> := [1,2,3]; using var f := "
            "FrozenArray<int>.From(a); Console.WriteLine(f.Peek()); "
            "Console.WriteLine(f.Push(1)); "
            "Console.WriteLine(Mk.F(a).Peek()); return 0; end Main; end "
            "Program;"),
        "7\n8\nmk\n7\n");
}

// ---- controls: the annotated shape that always worked ----------------------

// §26.5 rule 2 (v0.355): the declaration-less annotation shape must keep
// dispatching (the fix widens the built-in classification, it must not
// narrow it).
BEZEL_TEST(extension_builtin_stub_receiver_7756, annotated_controls) {
    BEZEL_EXPECT_EQ(
        out(prog("extension TimeSpan public method Twice(): TimeSpan => "
                 "TimeSpan.FromTicks(this.Ticks * 2L); end; extension Guid "
                 "public method Short(): string => this.ToString(\"N\"); end;",
                 "var t: TimeSpan := TimeSpan.FromHours(1.0); "
                 "var g: Guid := Guid.Empty; "
                 "Console.WriteLine(t.Twice()); "
                 "Console.WriteLine(g.Short());")),
        "02:00:00\n00000000000000000000000000000000\n");
}

// §26.5 rule 2 (v0.355): a user CLASS named like no built-in still matches
// only its own extension -- an extension on the built-in `TimeSpan` must not
// reach a user type, and vice versa.
BEZEL_TEST(extension_builtin_stub_receiver_7756, user_class_not_matched_by_builtin_head) {
    BEZEL_EXPECT_EQ(
        runAotOut("module M; class Span public method Ticks(): long => 3L; end "
                  "Span; extension Span public method Twice(): long => "
                  "this.Ticks() * 2L; end; class C public static method "
                  "Main(): int var s := new Span(); Console.WriteLine("
                  "s.Twice()); return 0; end Main; end C;"),
        "6");
}

// ---- §26.5 rule 1 on the stub shape: a DECLARED built-in member wins -------
//
// §26.5 rule 1 (v0.355): "If `x`'s declared type (or any base type)
// *declares* any instance member named `Foo` -- of any arity, applicability,
// or accessibility -- instance resolution is committed and extension blocks
// are **never consulted** for that name on that receiver."  stdlib §2.5 /
// §4.19 (v0.355) declare `DateTime.AddDays(days: double): DateTime`,
// `DateTime.AddYears(years: int): DateTime`, `DateTime.Year: int`,
// `TimeSpan.TotalSeconds: double`, `Guid.ToByteArray(): owned Array<byte>`
// and `FrozenArray<T>.ToArray(): owned Array<T>`, plus §8.1's `ToString`.
// Matching the stub-typed receiver against an extension head (the rows
// above) must not let a same-named extension shadow those: with the
// matcher widened alone, sema typed the call by the extension's `int` while
// codegen lowered the built-in, so the program printed pointer bits at exit
// 0 (q1, q2, q5, p03a) or a valid program was rejected (q6, p03d).  Every
// expected line is the built-in's answer.

// The receiver is a built-in CALL RESULT local (`e := d.AddDays(1.0)`).
BEZEL_TEST(extension_builtin_stub_receiver_7756, rule1_dt_adddays_on_call_result_local) {
    BEZEL_EXPECT_EQ(
        out(prog("extension DateTime public method AddDays(n: double): int "
                 "=> 5; end;",
                 "var d := new DateTime(2024, 3, 16); "
                 "var e := d.AddDays(1.0); "
                 "Console.WriteLine(e.AddDays(1.0).ToString());")),
        "2024-03-18T00:00:00\n");
}

// The receiver is the static factory result `DateTime.FromTicks(0L)`.
BEZEL_TEST(extension_builtin_stub_receiver_7756, rule1_dt_adddays_addyears_on_factory) {
    BEZEL_EXPECT_EQ(
        out(prog("extension DateTime public method AddDays(n: double): int "
                 "=> 5; public method AddYears(n: int): int => 6; end;",
                 "Console.WriteLine(DateTime.FromTicks(0L).AddDays(1.0)"
                 ".ToString()); "
                 "Console.WriteLine(DateTime.FromTicks(0L).AddYears(1)"
                 ".ToString());")),
        "0001-01-02T00:00:00\n0002-01-01T00:00:00\n");
}

// The receiver is the static property `Guid.Empty`; `ToString` (§8.1 /
// stdlib §2.6) and `ToByteArray` are both declared.
BEZEL_TEST(extension_builtin_stub_receiver_7756, rule1_guid_tostring_tobytearray_on_static_property) {
    BEZEL_EXPECT_EQ(
        out(prog("extension Guid public method ToString(): string => "
                 "\"EXT\"; public method ToByteArray(): int => 3; end;",
                 "Console.WriteLine(Guid.Empty.ToString()); "
                 "Console.WriteLine(Guid.Empty.ToByteArray().ToString());")),
        "00000000-0000-0000-0000-000000000000\n"
        "System.Collections.Generic.Array<byte>\n");
}

// The receiver is the factory result `FrozenArray<int>.From(a)`; stdlib
// §4.19 declares `ToArray(): owned Array<T>`.
BEZEL_TEST(extension_builtin_stub_receiver_7756, rule1_frozenarray_toarray_on_factory) {
    BEZEL_EXPECT_EQ(
        out(prog("extension FrozenArray<int> public method ToArray(): int "
                 "=> 98; end;",
                 "var a: Array<int> := [1, 2, 3]; "
                 "using var f := FrozenArray<int>.From(a); "
                 "Console.WriteLine(f.ToArray().ToString());")),
        "System.Collections.Generic.Array<int>\n");
}

// The same, with the built-in's `owned Array<int>` result bound and used:
// the extension's `int` would make `b.Count` a reject.
BEZEL_TEST(extension_builtin_stub_receiver_7756, rule1_frozenarray_toarray_count_on_factory) {
    BEZEL_EXPECT_EQ(
        out(prog("extension FrozenArray<int> public method ToArray(): int "
                 "=> 98; end;",
                 "var a: Array<int> := [1, 2, 3]; "
                 "using var f := FrozenArray<int>.From(a); "
                 "using var b := f.ToArray(); "
                 "Console.WriteLine(b.Count);")),
        "3\n");
}

// A chain of built-in call results and factories: `ToString` and the
// property `TotalSeconds` on TimeSpan factories, `AddDays` twice and then
// the built-in `Year` on a DateTime call result.
BEZEL_TEST(extension_builtin_stub_receiver_7756, rule1_timespan_datetime_chains) {
    BEZEL_EXPECT_EQ(
        out(prog("extension TimeSpan public method ToString(): string => "
                 "\"EXT\"; public property TotalSeconds: double => 1.5; end; "
                 "extension DateTime public method ToString(): string => "
                 "\"EXT\"; public method AddDays(n: double): int => 5; end;",
                 "Console.WriteLine(TimeSpan.FromTicks(5L).ToString()); "
                 "Console.WriteLine(TimeSpan.FromHours(1.0).TotalSeconds); "
                 "var d := new DateTime(2024, 3, 16); "
                 "Console.WriteLine(d.AddDays(1.0).ToString()); "
                 "Console.WriteLine(d.AddDays(1.0).AddDays(1.0).Year);")),
        "00:00:00.0000005\n3600\n2024-03-17T00:00:00\n2024\n");
}

// §8.1's universal `GetHashCode` is declared by every built-in through its
// `Object` base, so rule 1 commits it on a `FrozenArray<int>.From(a)` stub
// too, as does stdlib §4.19's `Length`: the extension's `string` would make
// the `int` local a §5.1 reject.
BEZEL_TEST(extension_builtin_stub_receiver_7756, rule1_frozenarray_object_member_and_length_on_factory) {
    BEZEL_EXPECT_EQ(
        out(prog("extension FrozenArray<int> public method GetHashCode(): "
                 "string => \"EXT\"; public method Length(): string => "
                 "\"EXTLEN\"; end;",
                 "var a: Array<int> := [1, 2, 3]; "
                 "using var f := FrozenArray<int>.From(a); "
                 "Console.WriteLine(f.Length); "
                 "var h: int := f.GetHashCode(); "
                 "Console.WriteLine(h.ToString().Length > 0);")),
        "3\nTrue\n");
}

// A stub of a head whose declared surface sema does not tabulate
// (stdlib §8.1 `Stopwatch`, from `Stopwatch.StartNew()`) is NOT matched by
// name, since nothing would commit its declared members first.  stdlib §8.1
// declares `public readonly property IsRunning: bool`, so whatever the
// verdict on the CALL spelling `sw.IsRunning()` is, it is never the
// extension's answer.  MEASURED with the matcher widened to every stub, the
// program printed the extension's "EXT".  (The oracle is the extension's
// marker only: how the built-in call spelling itself is lowered is a
// separate question this row does not pin.)
BEZEL_TEST(extension_builtin_stub_receiver_7756, rule1_untabulated_stub_head_never_answers_declared_name) {
    auto r = runAot(
        "module M; import System.Diagnostics; extension Stopwatch public "
        "method IsRunning(): string => \"EXT\"; end; class C public static "
        "method Main(): int using var sw := Stopwatch.StartNew(); "
        "Console.WriteLine(sw.IsRunning().ToString()); return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.stdoutText.find("EXT") == std::string::npos);
}
