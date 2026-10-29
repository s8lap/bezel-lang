// Spec-derived regression test for issue #7928 (campaign builtin-call-admission,
// lane C2-51): a `long` / `uint` / `double` argument at an `int` parameter of
// the Span / ReadOnlySpan / ArraySpan / ReadOnlyArraySpan / ReadOnlyStringSpan
// windows and of `JsonOptions` (the #7820 class).
//
// --- THE NORMATIVE SENTENCES (CHECKED in docs/, v0.350) ----------------------
//
// stdlib §3.3 `ReadOnlyStringSpan(Source: string, Start: int, Length: int)`;
//   §4.14 `Span<T>` / `ReadOnlySpan<T>(Buffer: OwningBuffer<T>, Start: int,
//   Length: int)`; §4.x `ArraySpan<T>` / `ReadOnlyArraySpan<T>(Source:
//   Array<T>, Start: int, Length: int)`; §2.15 `JsonOptions(Indented: bool,
//   IndentWidth: int, AllowTrailingCommas: bool, IgnoreUnknownFields: bool,
//   MaxDepth: int)`.
// spec §5.1: no implicit narrowing and no signed/unsigned conversion of the
//   same or narrower width; an in-range constant converts (constant narrowing).
//
// --- THE DEFECT (MEASURED on r30-c1 26216e9f) --------------------------------
//
// `new Span<int>(a, s, 2L)` over `s: long := 4294967298` read a[2]: the `long`
// was silently truncated to the i32 window field.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& body) {
    return "module M; import System; import System.Text; "
           "import System.Serialization; "
           "class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

void expectRefused(const std::string& body, const char* param,
                   const char* head) {
    auto r = runAot(prog(body));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, (std::string("the `") + param + "` argument of the `" + head +
            "` constructor has a type that is not implicitly convertible "
            "to the target type (\xC2\xA7""5.1)").c_str()));
}

}  // namespace

BEZEL_TEST(builtin_ctor_window_json_arg_types_7928, span_long_start_refused) {
    expectRefused(
        "    var a := new OwningBuffer<int>(10); var s: long := 4294967298; "
        "    var sp := new Span<int>(a, s, 2); Console.WriteLine(sp.Length);",
        "Start", "Span");
}
BEZEL_TEST(builtin_ctor_window_json_arg_types_7928, span_long_length_refused) {
    expectRefused(
        "    var a := new OwningBuffer<int>(10); var l: long := 3; "
        "    var sp := new Span<int>(a, 1, l); Console.WriteLine(sp.Length);",
        "Length", "Span");
}
BEZEL_TEST(builtin_ctor_window_json_arg_types_7928, readonly_span_and_array_spans_refused) {
    expectRefused(
        "    var a := new OwningBuffer<int>(10); var l: long := 3; "
        "    var sp := new ReadOnlySpan<int>(a, l, 1);",
        "Start", "ReadOnlySpan");
    expectRefused(
        "    var a := new Array<int>(10, 1); var u: uint := 3; "
        "    var sp := new ArraySpan<int>(a, 1, u);",
        "Length", "ArraySpan");
    expectRefused(
        "    var a := new Array<int>(10, 1); var l: long := 3; "
        "    var sp := new ReadOnlyArraySpan<int>(a, l, 1);",
        "Start", "ReadOnlyArraySpan");
}
BEZEL_TEST(builtin_ctor_window_json_arg_types_7928, read_only_string_span_long_refused) {
    expectRefused(
        "    var s: long := 6; var l: long := 5; "
        "    var sp := new ReadOnlyStringSpan(\"hello world\", s, l); "
        "    Console.WriteLine(sp.Length);",
        "Start", "ReadOnlyStringSpan");
}
BEZEL_TEST(builtin_ctor_window_json_arg_types_7928, json_options_long_refused) {
    expectRefused(
        "    var w: long := 4294967298; "
        "    var o := new JsonOptions(true, w, false, false, 64); "
        "    Console.WriteLine(o.IndentWidth);",
        "IndentWidth", "JsonOptions");
    expectRefused(
        "    var d: long := 3; "
        "    var o := new JsonOptions(true, 2, false, false, d);",
        "MaxDepth", "JsonOptions");
}
// Controls: int, narrower integers and in-range constants still construct.
BEZEL_TEST(builtin_ctor_window_json_arg_types_7928, accept_controls) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var a := new Array<int>(10, 1); var b: byte := 2; var sh: short := 3; "
        "    var sp := new ArraySpan<int>(a, b, sh); Console.WriteLine(sp.Length); "
        "    var s: int := 6; "
        "    var r := new ReadOnlyStringSpan(\"hello world\", s, 5L); "
        "    Console.WriteLine(r.Length); "
        "    var w: short := 4; "
        "    var o := new JsonOptions(true, w, false, false, 64); "
        "    Console.WriteLine(\"json\");")),
        "3\n5\njson");
}
