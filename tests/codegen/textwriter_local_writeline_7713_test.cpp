// Regression for #7713 -- `var w: TextWriter := Console.Out; w.WriteLine(x)`
// failed to compile: "`WriteLine` on built-in type `TextWriter` produced no
// value, and no dispatch arm emitted any code for this call" -- for EVERY
// argument (int, string, Object, class ... the owning-choice probe was one
// instance), because a `TextWriter`-typed receiver had no lowering at all.
//
// per stdlib §7.4 (v0.350): `TextWriter` declares `Write(string)`, `Write(char)`,
// `Write(Object)`, `WriteLine()`, `WriteLine(string)`, `WriteLine(Object)`, the
// per-type bool/numeric overloads and the abstract `Flush()`; "`TextWriter`'s
// `WriteLine` members write the argument followed by `Environment.NewLine`, the
// no-argument form writing the terminator alone, and its `Write` members write
// no terminator -- stdlib §2.1's rule, of which `Console.Out` and
// `Console.Error` are instances". So a `TextWriter` bound to `Console.Out` /
// `Console.Error` writes what `Console.Out.<m>` / `Console.Error.<m>` write.
// Scope of the fix: a LOCAL whose initializer is `Console.Out` / `Console.Error`
// and which is never written again; any other receiver keeps the #2457 reject.
// The `Object` overload is a boxing position, so an owning choice / owning
// inline array there is rejected (spec §35.8 / §8.6 / §15.3, #7694) and a
// borrow-bearing value too (spec §9.7).

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
const char* kPre =
    "module M; "
    "class Res public constructor() end constructor; end Res; "
    "class Cls public constructor() end constructor; "
    "  public override method ToString(): string => \"cls\"; end Cls; "
    "record Rec(x: int); "
    "choice OC Text(owned r: Res), Empty; end OC; "
    "choice VC A(x: int), B; end VC; "
    "class Bx public owned V: OC := OC.Empty; end Bx; "
    "class P public static method Main() ";

bool rejectedAsBox(const std::string& body) {
    auto r = runAot(std::string(kPre) + body + " end Main; end P;");
    return !r.ran && bezel::test::diagnosticMessageContains(r, "cannot be boxed");
}
}  // namespace

// The value-like controls the issue asked for: int, string, Object, class,
// plus bool/double/record/value-like choice/unit/no-argument. WriteLine form.
BEZEL_TEST(textwriter_local_writeline_7713, writeline_value_controls) {
    auto r = runAot(std::string(kPre) +
        "var w: TextWriter := Console.Out; var o: Object := 7; var c := new Cls(); "
        "var rc := new Rec(4); var u: () := (); "
        "w.WriteLine(5); w.WriteLine(\"s\"); w.WriteLine(o); w.WriteLine(c); "
        "w.WriteLine(rc); w.WriteLine(true); w.WriteLine(2.5); w.WriteLine(VC.A(1)); "
        "w.WriteLine(u); w.WriteLine(); w.WriteLine('x'); "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
        std::string("5\ns\n7\ncls\nRec(x: 4)\nTrue\n2.5\nVC.A(x: 1)\n()\n\nx\n"));
}

// Write form: no terminator.
BEZEL_TEST(textwriter_local_writeline_7713, write_value_controls) {
    auto r = runAot(std::string(kPre) +
        "var w: TextWriter := Console.Out; var o: Object := 7; var c := new Cls(); "
        "w.Write(5); w.Write(\"s\"); w.Write(o); w.Write(c); w.Write(new Rec(4)); "
        "w.Write('x'); w.Write(false); w.Write(VC.B); "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5s7clsRec(x: 4)xFalseVC.B"));
}

// An interpolated string and a non-owning inline array (#7715) as the argument,
// and Flush().
BEZEL_TEST(textwriter_local_writeline_7713, interpolation_inline_array_and_flush) {
    auto r = runAot(std::string(kPre) +
        "var w: TextWriter := Console.Out; var a: [3]int := [1, 2, 3]; "
        "w.WriteLine($\"v={5} {VC.A(2)}\"); w.WriteLine(a); w.Flush(); "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("v=5 VC.A(x: 2)\n[3]int\n"));
}

// A local bound to Console.Error writes to stderr (fd 2): its text is absent
// from the captured stdout while the Console.Out local's is present.
BEZEL_TEST(textwriter_local_writeline_7713, error_local_writes_to_stderr_not_stdout) {
    auto r = runAot(std::string(kPre) +
        "var e: TextWriter := Console.Error; var w: TextWriter := Console.Out; "
        "e.WriteLine(\"to-err\"); e.Write(5); e.WriteLine(); e.Flush(); "
        "w.WriteLine(\"to-out\"); "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("to-out\n"));
}

// No per-call leak: a boxed Object and a concatenated string local written 200x
// (the writer is a call-bounded borrow, as for Console.WriteLine, #1994).
BEZEL_TEST(textwriter_local_writeline_7713, loop_no_leak) {
    auto r = runAot(std::string(kPre) +
        "var w: TextWriter := Console.Out; var o: Object := 7; "
        "var s := \"a\" + 5.ToString(); "
        "for var i in 0..<200 do w.Write(o); w.Write(s); w.WriteLine(o); end for; "
        "w.WriteLine(s); "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText.size(), std::size_t(200 * 5 + 3));
}

// The issue's repro (probe rej_tw.bzl): an owning choice is a boxing at the
// `Object` overload, rejected exactly as for Console (spec §35.8, #7694).
BEZEL_TEST(textwriter_local_writeline_7713, owning_choice_field_rejected) {
    BEZEL_EXPECT_TRUE(rejectedAsBox(
        "var bx := new Bx(); var w: TextWriter := Console.Out; w.WriteLine(bx.V);"));
}
BEZEL_TEST(textwriter_local_writeline_7713, owning_choice_write_rejected) {
    BEZEL_EXPECT_TRUE(rejectedAsBox(
        "var bx := new Bx(); var w: TextWriter := Console.Error; w.Write(bx.V);"));
}
BEZEL_TEST(textwriter_local_writeline_7713, owning_inline_array_rejected) {
    BEZEL_EXPECT_TRUE(rejectedAsBox(
        "var w: TextWriter := Console.Out; var a: [2 owned]Res? := [null, null]; "
        "w.WriteLine(a);"));
}

// A borrow-bearing argument (a span) may not reach the `Object` parameter
// (spec §9.7, #5659) -- the same rejection Console.WriteLine gives.
BEZEL_TEST(textwriter_local_writeline_7713, borrow_bearing_span_rejected) {
    auto r = runAot(std::string(kPre) +
        "var w: TextWriter := Console.Out; var ar: Array<int> := [1, 2, 3]; "
        "var s := ar.AsSpan(); w.WriteLine(s); end Main; end P;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, "borrow-erasing"));
}
