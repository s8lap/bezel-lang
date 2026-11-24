// fix #8942 (§23.3, §27.5, §28.9, v0.359): an inlined property SETTER whose
// lambda captures the setter parameter `value` released a freshly produced
// string argument once too often, a heap-use-after-free after
// `h.P := "v" + 5.ToString();`.
//
// Spec (v0.359):
//  - §23.3: "The implicit setter parameter is `value`. It is a parameter of
//    the setter" — it behaves as an ordinary string parameter.
//  - §27.5: a closure heap-allocates the variables it captures; the capture
//    cell owns what it holds and releases it when the closure is reclaimed.
//  - §28.9 / §8.1: a string stored in a field is retained by the store.
//
// THE DEFECT. A method taking `value: string` retains it at the capture
// (#2689); the inlined setter binds `value` as a local anchored on the
// PropertyDecl, so no capture-time retain was emitted although the closure's
// environment destructor releases the cell's string unconditionally. With a
// literal argument the release hit an immortal string; with a concatenation
// it freed the caller's only reference. MEASURED on r30-c1 (lane
// r43-c10-8944), the issue's repro: AddressSanitizer heap-use-after-free; the
// method twin `Set(value: string)` is clean.
//
// ORACLE. `runAotChecked` (generated-code ASan + LeakSanitizer): the rows
// assert exact stdout, exit code, no ASan report and no leak, looped 200x.
//
// per §23.3, §27.5, §28.9 (v0.359)

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
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary = bezel::test::aot_detail::parseLeakSummary(
        text, leakedBytes, leakedAllocs);
    BEZEL_EXPECT_EQ(sawSummary ? leakedAllocs : 0ull, 0ull);
    BEZEL_EXPECT_EQ(sawSummary ? leakedBytes : 0ull, 0ull);
}

}  // namespace

// THE REGRESSION ROW: the issue's repro, its single store looped 200x. Each
// pass stores a fresh `"v" + i`; the total of the stored lengths is
// 10*2 + 90*3 + 100*4 = 690.
BEZEL_TEST(setter_value_capture_fresh_string_arg_8942,
           setter_lambda_capturing_value_fresh_concat_200x) {
    expectClean(runAotChecked(
        "module M; "
        "public class H "
        "  public s: string := \"\"; "
        "  public property P: string "
        "    get => s; "
        "    set => do var k := lambda(): string => value; s := k(); end; "
        "  end; "
        "end H; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    using var h := new H(); "
        "    var tot := 0; "
        "    for var i in 0..<200 do "
        "      h.P := \"v\" + i.ToString(); tot := tot + h.s.Length; "
        "    end; "
        "    Console.Write(tot); "
        "    return 0; "
        "  end; "
        "end Program;"),
        "690");
}

// The issue's literal one-shot program (value `"v5"`, length 2), kept beside
// the looped row.
BEZEL_TEST(setter_value_capture_fresh_string_arg_8942,
           setter_lambda_capturing_value_issue_repro_verbatim) {
    expectClean(runAotChecked(
        "module M; "
        "public class H "
        "  public s: string := \"\"; "
        "  public property P: string "
        "    get => s; "
        "    set => do var k := lambda(): string => value; s := k(); end; "
        "  end; "
        "end H; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    using var h := new H(); "
        "    h.P := \"v\" + 5.ToString(); "
        "    Console.Write(h.s.Length); "
        "    return 0; "
        "  end; "
        "end Program;"),
        "2");
}
