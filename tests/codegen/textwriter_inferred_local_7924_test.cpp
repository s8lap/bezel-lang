// Regression for #7924 -- `var w := Console.Out; w.Write(o)` (the local's type
// INFERRED from `Console.Out`) wrote nothing and leaked the boxed `Object`
// argument (24 B), while the annotated `var w: TextWriter := Console.Out` form
// was clean (#7713).
//
// per stdlib §2.1 (v0.350): `Console.Out` and `Console.Error` are static
// readonly properties of type `TextWriter`; per spec §21.1 a `var` local with
// no annotation takes the initializer's type. `Console.Out` has NO type in
// sema (its chained `Console.Out.Write` paths are syntactic), so the inferred
// local had none either and no `TextWriter` arm ever matched `w.Write(...)` --
// the call lowered to nothing and the argument's box was never released. An
// unannotated local (`var` or `const`) initialized by `Console.Out` /
// `Console.Error` now takes `TextWriter`, the same thing the annotated one is.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
const char* kPre =
    "module M; "
    "class Cls public constructor() end constructor; "
    "  public override method ToString(): string => \"cls\"; end Cls; "
    "class P public static method Main() ";
}  // namespace

// The issue's repro (v2var.bzl): the boxed Object argument prints and is freed.
BEZEL_TEST(textwriter_inferred_local_7924, issue_repro_prints_and_no_leak) {
    auto r = runAotChecked(
        "module M; public class Program "
        "public static method Main(): int "
        "var w := Console.Out; var o: Object := 7; w.Write(o); return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7"));
}

// Loop the leak repro 200x (a one-shot can hide under sanitizer noise).
BEZEL_TEST(textwriter_inferred_local_7924, loop_no_leak) {
    auto r = runAotChecked(std::string(kPre) +
        "var w := Console.Out; var o: Object := 7; var s := \"a\" + 5.ToString(); "
        "for var i in 0..<200 do w.Write(o); w.Write(s); w.WriteLine(o); end for; "
        "w.WriteLine(s); end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText.size(), std::size_t(200 * 5 + 3));
}

// Every spelling of an inferred local: var / const, Out / Error.
BEZEL_TEST(textwriter_inferred_local_7924, var_const_out_error_matrix) {
    auto r = runAotChecked(std::string(kPre) +
        "var w := Console.Out; const c := Console.Out; "
        "var e := Console.Error; const ce := Console.Error; var o: Object := 7; "
        "w.WriteLine(5); w.Write(\"s\"); w.WriteLine(o); var k := new Cls(); c.WriteLine(k); "
        "c.Write('x'); c.WriteLine(); w.Flush(); "
        "e.WriteLine(\"to-err\"); ce.Write(o); e.Flush(); "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\ns7\ncls\nx\n"));
}

// An inferred local that is reassigned writes to the stream it holds at the
// write, exactly as the annotated one does. fix #7792 (stdlib §2.1 / §7.4,
// v0.355) flipped this row: it pinned the #2457 reject of a reassigned local,
// which only existed because a `TextWriter` value had no representation. The
// stream follows the value now, so `w := Console.Error; w.WriteLine(1)` writes
// to stderr (the harness captures stdout only) and the later write to
// `Console.Out` reaches stdout.
BEZEL_TEST(textwriter_inferred_local_7924, reassigned_inferred_local_follows_the_value) {
    auto ann = runAotChecked(std::string(kPre) +
        "var w: TextWriter := Console.Out; w := Console.Error; w.WriteLine(1); "
        "w := Console.Out; w.WriteLine(2); end Main; end P;");
    auto inf = runAotChecked(std::string(kPre) +
        "var w := Console.Out; w := Console.Error; w.WriteLine(1); "
        "w := Console.Out; w.WriteLine(2); end Main; end P;");
    BEZEL_EXPECT_TRUE(ann.ran);
    BEZEL_EXPECT_TRUE(inf.ran);
    BEZEL_EXPECT_EQ(ann.stdoutText, std::string("2\n"));
    BEZEL_EXPECT_EQ(inf.stdoutText, std::string("2\n"));
}

// The direct, unbound spellings are untouched by the inferred-local typing:
// `Console.Out.Flush()` compiles (as a no-op) and the chained writes keep
// printing, including a narrowed nullable argument.
BEZEL_TEST(textwriter_inferred_local_7924, direct_console_out_members_unchanged) {
    auto r = runAotChecked(std::string(kPre) +
        "Console.Out.Flush(); Console.Error.Flush(); Console.Out.Write(\"a\"); "
        "Console.Out.WriteLine(); Console.Out.WriteLine(\"b\"); "
        "Console.Error.WriteLine(\"e\"); var o: Object := 3; "
        "Console.Out.WriteLine(o); end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("a\nb\n3\n"));
}
