// fix #7001 -- codegen: `Encoding.<singleton>` used as a VALUE and `.Name`
// read through it (stdlib §3.2, v0.340: the static properties return
// singleton instances accessed through the `Encoding` base type). The value
// failed to lower, so `e.Name` printed an empty line / the program exited 0
// with no output. Exact stdout per row; local, parameter and field routes;
// UTF8 / UTF16 / UTF32 / ASCII (the stdlib's four singletons).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
std::string run(const std::string& body, const std::string& members = "") {
    auto r = runAotChecked(
        "module M; import System; import System.Text; "
        "class H public Enc: Encoding; "
        "  public constructor(e: Encoding) this.Enc := e; end constructor; end H; "
        "class C " + members +
        "  static method Pn(e: Encoding): string return e.Name; end Pn; "
        "  public static method Main(): int " + body + " return 0; end Main; end C;");
    if (!r.ran || r.exitCode != 0) return "<did-not-run>";
    return r.stdoutText;
}
}  // namespace

BEZEL_TEST(encoding_name_through_value_7001, local_utf8_issue_repro_3_2) {
    BEZEL_EXPECT_EQ(run("var e: Encoding := Encoding.UTF8; Console.WriteLine(e.Name);"),
                    std::string("utf-8\n"));
}
BEZEL_TEST(encoding_name_through_value_7001, local_all_four_3_2) {
    BEZEL_EXPECT_EQ(run(
        "var a: Encoding := Encoding.UTF8; var b: Encoding := Encoding.UTF16; "
        "var c: Encoding := Encoding.UTF32; var d: Encoding := Encoding.ASCII; "
        "Console.WriteLine(a.Name); Console.WriteLine(b.Name); "
        "Console.WriteLine(c.Name); Console.WriteLine(d.Name);"),
        std::string("utf-8\nutf-16le\nutf-32le\nus-ascii\n"));
}
BEZEL_TEST(encoding_name_through_value_7001, parameter_3_2) {
    BEZEL_EXPECT_EQ(run(
        "Console.WriteLine(Pn(Encoding.ASCII)); "
        "var e: Encoding := Encoding.UTF16; Console.WriteLine(Pn(e));"),
        std::string("us-ascii\nutf-16le\n"));
}
BEZEL_TEST(encoding_name_through_value_7001, field_3_2) {
    BEZEL_EXPECT_EQ(run(
        "var h := new H(Encoding.UTF32); Console.WriteLine(h.Enc.Name);"),
        std::string("utf-32le\n"));
}
// Control: the direct spelling keeps working.
BEZEL_TEST(encoding_name_through_value_7001, direct_spelling_unchanged_3_2) {
    BEZEL_EXPECT_EQ(run("Console.WriteLine(Encoding.UTF8.Name); "
                        "Console.WriteLine(Encoding.ASCII.Name);"),
                    std::string("utf-8\nus-ascii\n"));
}
