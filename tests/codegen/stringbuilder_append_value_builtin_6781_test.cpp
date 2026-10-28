// Spec-derived regression test for issue #6781.
//
// stdlib §3.1 (StringBuilder), v0.340: "observable behavior = Append(value.
// ToString()); no Object boxing on the path." A VALUE-kind argument must append
// exactly what its own ToString() returns. Before the fix a TimeSpan / DateTime /
// DateOnly / TimeOnly / enum argument appended its raw integer (tick count, day
// number, ms-of-day, ordinal) and a Guid / record argument produced malformed IR.
//
// Each row compares Append(x) / AppendLine(x.ToString()) with
// Append(x.ToString()) / AppendLine(x.ToString()) inside ONE program, so it does
// not depend on the text the type renders (#6720 for TimeSpan).
//
// fix #7420 (stdlib §3.1, v0.345): the `AppendLine` half of this program used
// to pass the VALUE kind itself (`a.AppendLine(x)`). §3.1 declares EXACTLY
// two `AppendLine` forms — `AppendLine(): StringBuilder` and
// `AppendLine(value: string): StringBuilder` — so a value kind reaches no
// declared `AppendLine` form (§22.6) and an `int` reaches a `string` parameter
// only by an explicit conversion (§5.1). The `AppendLine` calls therefore take
// the STRING path (`x.ToString()`), which §3.1 declares; the `Append` calls
// keep the value kind, because §3.1 declares SEVEN `Append` overloads
// (including `Append(value: Object)`). The `Append` half is what #6781 is
// about, and it is unchanged.
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

std::string prog(const std::string& prelude, const std::string& init) {
    return "module M; " + prelude +
           " class C public static method Main(): int "
           "  var x := " + init + "; "
           "  var i := 0; var ok := true; "
           "  while i < 50 do "
           "    var a := new StringBuilder(); "
           "    a.Append(x); a.AppendLine(x.ToString()); a.Append(x); "
           "    var b := new StringBuilder(); "
           "    b.Append(x.ToString()); b.AppendLine(x.ToString()); b.Append(x.ToString()); "
           "    if a.ToString() <> b.ToString() then ok := false; end; "
           "    i := i + 1; "
           "  end; "
           "  var s := new StringBuilder(); s.Append(x); "
           "  if ok and s.ToString() = x.ToString() and s.Length > 0 "
           "  then Console.WriteLine(\"EQ\"); "
           "  else Console.WriteLine(\"NE \" + s.ToString() + \" / \" + x.ToString()); end; "
           "  return 0; "
           "end Main; end C;";
}

void expectEq(const std::string& prelude, const std::string& init) {
    auto r = runAotChecked(prog(prelude, init));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("EQ\n"));
}

} // namespace

// stdlib §3.1 / §2.5, v0.340 (issue #6781 repro): TimeSpan.
BEZEL_TEST(codegen_stringbuilder_append_value_builtin_6781, timespan) {
    expectEq("", "TimeSpan.FromSeconds(90.5)");
}
// stdlib §3.1 / §2.5, v0.340: DateTime.
BEZEL_TEST(codegen_stringbuilder_append_value_builtin_6781, datetime) {
    expectEq("", "DateTime.FromTicks(637000000000000000L)");
}
// stdlib §3.1 / §2.5-§2.6, v0.340: DateOnly.
BEZEL_TEST(codegen_stringbuilder_append_value_builtin_6781, dateonly) {
    expectEq("", "DateOnly.Parse(\"2019-07-29\")");
}
// stdlib §3.1 / §2.5-§2.6, v0.340: TimeOnly.
BEZEL_TEST(codegen_stringbuilder_append_value_builtin_6781, timeonly) {
    expectEq("", "TimeOnly.Parse(\"12:26:40\")");
}
// stdlib §3.1 / §2.6, v0.340: Guid (was malformed IR).
BEZEL_TEST(codegen_stringbuilder_append_value_builtin_6781, guid) {
    expectEq("", "Guid.Parse(\"12345678-1234-1234-1234-123456789abc\")");
}
// stdlib §3.1 / language spec §8.5, v0.340: enum (was its ordinal).
BEZEL_TEST(codegen_stringbuilder_append_value_builtin_6781, enum_value) {
    expectEq("public enum Color Red, Green; end Color;", "Color.Green");
}
// stdlib §3.1 / language spec §8.5, v0.340: record (was malformed IR).
BEZEL_TEST(codegen_stringbuilder_append_value_builtin_6781, record_value) {
    expectEq("public record P(x: int, y: int);", "new P(3, 4)");
}
