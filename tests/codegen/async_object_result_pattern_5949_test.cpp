// Codegen / soundness - issue #5949 (spec §30.5, v0.338): the Pattern A
// producer row reads "`Task<T>` for class or interface T (`Object`, a class
// (§3.1), included ...)" and the Pattern B row "`OwningTask<T>` for class or
// interface T". `Object` is a Primitive-kind TypeRef with no declaration, so
// `taskResultArgIsClass` never classified `Task<Object>` as Pattern A nor
// `OwningTask<Object>` as Pattern B: the unbound `var r := await Make();`
// (and `Make().Result`, and the OwningTask unbound/argument forms) were
// accepted and leaked the `new Object()` result under LeakSanitizer, where the
// class-typed twin is rejected by the owning-location rule.
//
// Companion to async_unbound_await_interface_result_5723_test.cpp. The
// remaining leak of a `new Object()` result through a BOUND `Task<Object>`
// (task teardown does not dispose an Object result) is a separate codegen
// cause and is deliberately not asserted here.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

constexpr const char* kPatternAOwningLocation =
    "awaiting a class-typed `Task<T>` (Pattern A) requires the awaited task to "
    "occupy a syntactically-visible owning location";
constexpr const char* kPatternAResultOwningLocation =
    "reading `.Result` on a class-typed `Task<T>` (Pattern A) requires the "
    "task expression to occupy a syntactically-visible owning location";
constexpr const char* kDroppedWithoutDisposal =
    "owned value in `r` would be dropped here without disposal";
constexpr const char* kPatternBConsumingContext =
    "awaiting an `OwningTask<T>` (Pattern B) must appear in an owned-consuming "
    "context";

std::string src(const std::string& members, const std::string& mainSig,
                const std::string& body) {
    return "module M; public class Program " + members +
           "  public static " + mainSig + " " + body + " end Main; "
           "end Program;";
}

constexpr const char* kTaskMake =
    "public static async method Make(): Task<Object> return new Object(); "
    "end Make; ";
constexpr const char* kOwningMake =
    "public static async method Make(): OwningTask<Object> "
    "return new Object(); end Make; ";

}  // namespace

BEZEL_TEST(async_object_result_pattern_5949,
          reject_unbound_var_await_task_object_30_5) {
    auto r = runAot(src(kTaskMake, "async method Main(): Task<int>",
                        "var r := await Make(); return 4;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kPatternAOwningLocation));
}

BEZEL_TEST(async_object_result_pattern_5949,
          reject_result_read_task_object_30_5) {
    auto r = runAot(src(kTaskMake, "method Main(): int",
                        "var r := Make().Result; return 4;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, kPatternAResultOwningLocation));
}

BEZEL_TEST(async_object_result_pattern_5949,
          reject_unbound_var_await_owningtask_object_30_5) {
    auto r = runAot(src(kOwningMake, "async method Main(): Task<int>",
                        "var r := await Make(); return 4;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kDroppedWithoutDisposal));
}

BEZEL_TEST(async_object_result_pattern_5949,
          reject_await_argument_owningtask_object_30_5) {
    auto r = runAot(src(
        std::string(kOwningMake) +
            "public static method Use(o: Object): int return 4; end Use; ",
        "async method Main(): Task<int>", "return Use(await Make());"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kPatternBConsumingContext));
}

// Admitted forms stay admitted and clean: a runtime-managed operand (a string
// boxed under Object) is kind 1 with nothing to dispose.
BEZEL_TEST(async_object_result_pattern_5949,
          accept_bound_await_task_object_string_looped_30_5) {
    const std::string s = src(
        "public static async method Make(): Task<Object> return \"abcd\"; "
        "end Make; ",
        "async method Main(): Task<int>",
        "var i: int := 0; var n: int := 0; "
        "while i < 60 do "
        "  do using var t := Make(); var r := await t; n := n + 4; end; "
        "  i := i + 1; "
        "end while; "
        "Console.WriteLine(n); return 0;");
    JitResult r = runAotChecked(s);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("240\n"));
}

BEZEL_TEST(async_object_result_pattern_5949,
          accept_using_var_await_owningtask_object_looped_30_5) {
    const std::string s = src(
        kOwningMake, "async method Main(): Task<int>",
        "var i: int := 0; var n: int := 0; "
        "while i < 60 do "
        "  do using var r := await Make(); n := n + 4; end; "
        "  i := i + 1; "
        "end while; "
        "Console.WriteLine(n); return 0;");
    JitResult r = runAotChecked(s);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("240\n"));
}
