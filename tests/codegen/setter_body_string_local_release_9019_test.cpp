// narrow #9019 F2 (§23.3, §28.9, v0.359): a `string` local declared in an
// INLINED block setter body (`set => do ... end;`) is released at the body's
// end. Before the fix it never was, so every assignment leaked it.
//
// Spec (v0.359):
//  - §23.3: "The implicit setter parameter is `value`. It is a parameter of
//    the setter", and it "takes a borrow of the property's declared type".
//  - §28.9: a runtime-managed `string` is reclaimed when its last reference
//    dies. A setter-body local holding a reference is such a reference.
//
// THE DEFECT. The issue reported F2 as a STATIC setter whose lambda captures
// `value` and leaks 7290 B in 400 allocations, which looked like a regression
// from #8942's capture retain of `value`. MEASURED on r30-c1 (BEZEL_DUMP_IR=1),
// the static path takes no extra reference. The INSTANCE twin leaks the same
// 7290 B, and so does a lambda-free `var r := value + "q";`. The reference
// that is never released belongs to the setter's own LOCAL `r` (the +1 that
// `k()` returns): no `bz_string_release` of it is emitted, because the inlined
// setter body was never string-analysed. Only function, lambda, constructor,
// iterator and async bodies run collectStringReleaseEligible, and the
// enclosing body's sets name only that body's LocalDecls. #8942's retain is
// correct. Before it, the envdtor's over-release of the caller's `value`
// happened to cancel the leak of `r`.
//
// The first attempt (e004c4eef) removed the retain for static setters. It was
// REJECTED: with a lambda that is never called, the envdtor then over-released
// the caller's string, and with a borrowed caller local the envdtor freed it
// (a heap-use-after-free). The rows below therefore cover the never-called,
// once-called, thrice-called, borrowed-local and fresh-argument shapes. Each
// row is red on r30-c1.
//
// ORACLE. `runAotChecked` (generated-code ASan + LeakSanitizer): each row
// asserts exact stdout, exit code 0, no ASan report and no leak, over 200
// assignments.
//
// per §23.3, §28.9 (v0.359)

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

void expectClean(const JitResult& r, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") ==
                      std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("over-release") == std::string::npos);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary = bezel::test::aot_detail::parseLeakSummary(
        text, leakedBytes, leakedAllocs);
    BEZEL_EXPECT_EQ(sawSummary ? leakedAllocs : 0ull, 0ull);
    BEZEL_EXPECT_EQ(sawSummary ? leakedBytes : 0ull, 0ull);
}

// `"v" + i` for i in 0..<200 has length 2 (10 times), 3 (90), 4 (100). Each
// setter body below prints one number per assignment; these are the expected
// streams.
std::string perLength(const std::string& a, const std::string& b,
                      const std::string& c) {
    std::string out;
    for (int i = 0; i < 200; ++i) out += (i < 10 ? a : (i < 100 ? b : c));
    return out;
}

// A static property `P` on class `H` with the given setter body, assigned the
// fresh concatenation `"v" + i` 200 times.
std::string staticProgram(const std::string& setterBody) {
    return "module M; "
           "public class H "
           "  public static property P: string "
           "    get => \"x\"; "
           "    set => do " + setterBody + " end; "
           "  end; "
           "end H; "
           "public static class Program "
           "  public static method Main(args: Array<string>): int "
           "    for var i in 0..<200 do H.P := \"v\" + i.ToString(); end; "
           "    return 0; "
           "  end; "
           "end Program;";
}

}  // namespace

// The lambda over `value` is created and NEVER called. The rejected attempt
// over-released here (p01 / q03 / q05 / q06 of its review). The body's own
// local `t` is what leaked on r30-c1.
BEZEL_TEST(setter_body_string_local_release_9019,
           static_setter_lambda_never_called_local_released_200x) {
    expectClean(
        runAotChecked(staticProgram(
            "var k := lambda(): string => value; "
            "var t := value + \"!\"; Console.Write(t.Length);")),
        perLength("3", "4", "5"));
}

// THE ISSUE'S SHAPE: the lambda is called once and its result is bound to a
// local. r30-c1 leaked 7290 B in 400 allocations.
BEZEL_TEST(setter_body_string_local_release_9019,
           static_setter_lambda_called_once_result_released_200x) {
    expectClean(
        runAotChecked(staticProgram(
            "var k := lambda(): string => value; "
            "var r := k(); Console.Write(r.Length);")),
        perLength("2", "3", "4"));
}

// Called three times, three result locals. The rejected attempt still leaked
// here.
BEZEL_TEST(setter_body_string_local_release_9019,
           static_setter_lambda_called_three_times_results_released_200x) {
    expectClean(
        runAotChecked(staticProgram(
            "var k := lambda(): string => value; "
            "var a := k(); var b := k(); var c := k(); "
            "Console.Write(a.Length + b.Length + c.Length);")),
        perLength("6", "9", "12"));
}

// The argument is a BORROWED caller local that the caller reads after the
// store. The rejected attempt freed it in the envdtor (q02, a
// heap-use-after-free). Each pass prints the setter's `r.Length`, then the
// caller's `s.Length`.
BEZEL_TEST(setter_body_string_local_release_9019,
           static_setter_borrowed_caller_local_survives_200x) {
    expectClean(
        runAotChecked(
            "module M; "
            "public class H "
            "  public static property P: string "
            "    get => \"x\"; "
            "    set => do var k := lambda(): string => value; "
            "      var r := k(); Console.Write(r.Length); end; "
            "  end; "
            "end H; "
            "public static class Program "
            "  public static method Main(args: Array<string>): int "
            "    for var i in 0..<200 do "
            "      var s := \"v\" + i.ToString(); H.P := s; "
            "      Console.Write(s.Length); "
            "    end; "
            "    return 0; "
            "  end; "
            "end Program;"),
        perLength("22", "33", "44"));
}

// A fresh concatenated argument and no lambda at all: the setter's own
// concatenation local is the whole defect.
BEZEL_TEST(setter_body_string_local_release_9019,
           static_setter_fresh_concat_argument_local_concat_released_200x) {
    expectClean(
        runAotChecked(staticProgram(
            "var r := value + \"q\"; Console.Write(r.Length);")),
        perLength("3", "4", "5"));
}

// The INSTANCE twin of the issue's shape. It leaked identically on r30-c1, so
// the defect is not specific to static setters.
BEZEL_TEST(setter_body_string_local_release_9019,
           instance_setter_lambda_called_once_result_released_200x) {
    expectClean(
        runAotChecked(
            "module M; "
            "public class H "
            "  public property P: string "
            "    get => \"x\"; "
            "    set => do var k := lambda(): string => value; "
            "      var r := k(); Console.Write(r.Length); end; "
            "  end; "
            "end H; "
            "public static class Program "
            "  public static method Main(args: Array<string>): int "
            "    using var h := new H(); "
            "    for var i in 0..<200 do h.P := \"v\" + i.ToString(); end; "
            "    return 0; "
            "  end; "
            "end Program;"),
        perLength("2", "3", "4"));
}

// The INDEXER twin (lowerIndexerSet's inlined body). r30-c1 leaked 7780 B in
// 400 allocations. `k() + i` has length 3 (i < 10), 5 (i < 100), 7.
BEZEL_TEST(setter_body_string_local_release_9019,
           indexer_setter_lambda_result_local_released_200x) {
    expectClean(
        runAotChecked(
            "module M; "
            "public class H "
            "  public property [i: int]: string "
            "    get => \"x\"; "
            "    set => do var k := lambda(): string => value; "
            "      var r := k() + i.ToString(); Console.Write(r.Length); end; "
            "  end; "
            "end H; "
            "public static class Program "
            "  public static method Main(args: Array<string>): int "
            "    using var h := new H(); "
            "    for var i in 0..<200 do h[i] := \"v\" + i.ToString(); end; "
            "    return 0; "
            "  end; "
            "end Program;"),
        perLength("3", "5", "7"));
}
