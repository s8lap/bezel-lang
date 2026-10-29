// Issue #7778 -- `?.` property reads on a nullable builtin value record.
//
// spec §7.5 (v0.350): `x?.M` evaluates to `null` when `x` is null, otherwise to
// the plain `x.M` lifted to the nullable form of M's type. stdlib §2.5 (v0.350):
// DateTime / TimeSpan / DateOnly / TimeOnly declare the read-only properties
// (Year, Ticks, Days, TotalHours, Hour, ...). Before the fix `a?.Year` over a
// `DateTime?` (and the TimeSpan?, DateOnly?, TimeOnly? siblings) was untyped by
// sema and lowered to nothing: the enclosing store was dropped (the #2434 /
// #3079 backstop) and the program printed nothing, exit 0.
//
// §7 forbids printing a `T?` directly, so each read is consumed with `??`.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// A genuine compile-time rejection is told from a crash by the diagnostic text.
bool hasError(const std::string& program, const char* keyword) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
                d.message.find(keyword) != std::string::npos)
            return true;
    }
    return false;
}

std::string prog(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           " return 0; end Main; end C;";
}

}  // namespace

// §7.5 / stdlib §2.5: the issue's repro -- `a?.Year` and `a?.Ticks`, non-null
// and null receivers.
BEZEL_TEST(nullcond_builtin_value_member_7778, spec_7_5_datetime_year_ticks_present_and_absent) {
    auto r = runAot(prog(
        "var a: DateTime? := new DateTime(2020, 1, 2, 3, 4, 5); "
        "var n: DateTime? := null; "
        "Console.WriteLine(a?.Year ?? -1); "
        "Console.WriteLine(a?.Month ?? -1); "
        "Console.WriteLine(a?.Day ?? -1); "
        "Console.WriteLine(a?.Hour ?? -1); "
        "Console.WriteLine(n?.Year ?? -1); "
        "Console.WriteLine(n?.Ticks ?? -1L); "
        "if (a?.Ticks ?? -1L) = a!.Ticks then Console.WriteLine(\"ticks ok\"); end if; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "2020\n1\n2\n3\n-1\n-1\nticks ok\n");
}

// §7.5: the result is a `T?` whatever the receiver -- it can be stored, tested
// for null, and chained with a second `?.`.
BEZEL_TEST(nullcond_builtin_value_member_7778, spec_7_5_result_is_nullable_and_chains) {
    auto r = runAot(prog(
        "var a: DateTime? := new DateTime(2020, 1, 2, 3, 4, 5); "
        "var n: DateTime? := null; "
        "var y: int? := a?.Year; "
        "var z: int? := n?.Year; "
        "if y <> null then Console.WriteLine(y); end if; "
        "if z = null then Console.WriteLine(\"null\"); end if; "
        "Console.WriteLine(a?.AddDays(1.0)?.Year ?? -1); "
        "Console.WriteLine(n?.AddDays(1.0)?.Year ?? -1); "
        "Console.WriteLine(a?.Date?.Year ?? -1); "
        "Console.WriteLine(a?.TimeOfDay?.Hour ?? -1); "
        "Console.WriteLine(n?.Date?.Year ?? -1); "
        "var dd := a?.Date; "
        "Console.WriteLine(dd?.Month ?? -1); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "2020\nnull\n2020\n-1\n2020\n3\n-1\n1\n");
}

// stdlib §2.5 siblings: TimeSpan?, DateOnly?, TimeOnly?.
BEZEL_TEST(nullcond_builtin_value_member_7778, stdlib_2_5_timespan_dateonly_timeonly_present_and_absent) {
    auto r = runAot(prog(
        "var d1 := new DateTime(2020, 1, 1, 0, 0, 0); "
        "var d2 := new DateTime(2020, 1, 3, 12, 0, 0); "
        "var t: TimeSpan? := d2 - d1; "
        "var tn: TimeSpan? := null; "
        "Console.WriteLine(t?.Days ?? -1); "
        "Console.WriteLine(t?.Hours ?? -1); "
        "Console.WriteLine(int(t?.TotalHours ?? -1.0)); "
        "Console.WriteLine(tn?.Days ?? -1); "
        "Console.WriteLine(int(tn?.TotalHours ?? -1.0)); "
        "var o: DateOnly? := new DateOnly(2021, 3, 4); "
        "var on: DateOnly? := null; "
        "Console.WriteLine(o?.Year ?? -1); "
        "Console.WriteLine(o?.Month ?? -1); "
        "Console.WriteLine(o?.Day ?? -1); "
        "Console.WriteLine(on?.Year ?? -1); "
        "var m: TimeOnly? := new TimeOnly(5, 6, 7); "
        "var mn: TimeOnly? := null; "
        "Console.WriteLine(m?.Hour ?? -1); "
        "Console.WriteLine(m?.Minute ?? -1); "
        "Console.WriteLine(mn?.Hour ?? -1); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "2\n12\n60\n-1\n-1\n2021\n3\n4\n-1\n5\n6\n-1\n");
}

// §7.4 + §7.5: the narrowed spelling (`q.Year` after a null test) and the `?.`
// spelling agree, in a loop (the lowering runs per iteration).
BEZEL_TEST(nullcond_builtin_value_member_7778, spec_7_4_narrowed_read_agrees_with_nullcond_in_loop) {
    auto r = runAot(prog(
        "var q: DateTime? := new DateTime(2020, 1, 2, 3, 4, 5); "
        "var n := 0; var i := 0; "
        "while i < 100 do "
        "  if q <> null then "
        "    if q.Year = (q?.Year ?? -1) and q.Ticks = (q?.Ticks ?? -1L) then n := n + 1; end if; "
        "  end if; "
        "  i := i + 1; "
        "end while; "
        "Console.WriteLine(n); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "100\n");
}

// stdlib §2.5 (v0.350): Year / Hour / Days are read-only PROPERTIES, not
// methods. Invoking one on a factory-shaped value (`a.Date`, `a.TimeOfDay`,
// `b - a`: declaration = builtinClassSym) stays a compile error. Widening the
// `.` form's property gate to these heads (the `?.` read's twin) must not let a
// CALL through as a #605 warning at exit 0.
BEZEL_TEST(nullcond_builtin_value_member_7778, stdlib_2_5_property_invoked_as_method_on_factory_shape_is_rejected) {
    BEZEL_EXPECT_TRUE(hasError(prog(
        "var a := new DateTime(2020, 1, 2, 3, 4, 5); var d := a.Date; "
        "var x := d.Year(1); "), "`Year` on built-in type `DateOnly` produced no value"));
    BEZEL_EXPECT_TRUE(hasError(prog(
        "var a := new DateTime(2020, 1, 2, 3, 4, 5); var o := a.TimeOfDay; "
        "var x := o.Hour(1); "), "`Hour` on built-in type `TimeOnly` produced no value"));
    BEZEL_EXPECT_TRUE(hasError(prog(
        "var a := new DateTime(2020, 1, 2, 3, 4, 5); "
        "var b := new DateTime(2020, 1, 3, 5, 4, 5); var t := b - a; "
        "var x := t.Days(1); "), "`Days` on built-in type `TimeSpan` produced no value"));
}
