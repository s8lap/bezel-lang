// Codegen / soundness - issue #5723 (spec §30.5 "Result lifetime and task
// disposal", v0.338). Pattern A's producer row reads "`Task<T>` for **class or
// interface** T" and Pattern B's "`OwningTask<T>` for **class or interface**
// T"; the consumer-side Pattern A rule admits an await only from a
// syntactically-visible owning location, an unbound producer confined to the
// same full expression, and (flag-off, as shipped) rejects every other
// escaping bind - `var r := await Producer();` binds the borrow to no
// location (the scoped await-bind that would admit it for class T is the
// unshipped prototype of #3048). The ownership pass's class gate
// (`taskResultArgIsClass`) answered true for `class` only, so an awaited
// `Task<IShape>` was classified a value result: `var r := await MakeAsync();`
// was ACCEPTED, the temporary task was disposed at the end of the declaration
// (taking the interface-typed result with it) and `r.Area()` read freed
// memory (native exit 139; ASan heap-use-after-free in the Main resume
// function). `OwningTask<IShape>` was classified NotTask: `var r := await ...`
// and `_ := await ...` leaked the result (LSan, 16 bytes per call).
//
// MEASURED on the pre-fix tree: unbound `Task<IShape>`, `Task<IShape?>` and
// generic `MakeG<IShape>` -> UAF; `OwningTask<IShape>` unbound `var` and `_ :=`
// -> leak. The bound form `using var t := F(); var r := await t;` was and
// stays correct.
//
// The peer file async_task_interface_result_5723_test.cpp owns the
// `using var r := await` reject and the `OwningTask<IShape>` `using var` accept
// rows; they are not duplicated here.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

constexpr const char* kShapes =
    "module M; "
    "public interface IShape method Area(): int; end IShape; "
    "public class Sq: IShape public method Area(): int => 4; end Sq; ";

// §30.5 Pattern A consumer rule (the checkAwaitSitesExpr diagnostic).
constexpr const char* kPatternAOwningLocation =
    "awaiting a class-typed `Task<T>` (Pattern A) requires the awaited task to "
    "occupy a syntactically-visible owning location";
// §28.3: an owned Pattern B result bound to a plain local with no discharge.
constexpr const char* kDroppedWithoutDisposal =
    "owned value in `r` would be dropped here without disposal";

std::string program(const std::string& members, const std::string& loopBody) {
    return std::string(kShapes) +
           "public class Program " + members +
           "  public static async method Main(): Task<int> "
           "    var sum: int := 0; var i: int := 0; "
           "    while i < 60 do "
           "      do " + loopBody + " end; "
           "      i := i + 1; "
           "    end while; "
           "    Console.WriteLine(sum); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

std::string rejectProgram(const std::string& members, const std::string& body) {
    return std::string(kShapes) +
           "public class Program " + members +
           "  public static async method Main(): Task<int> " + body +
           "    return 0; end Main; end Program;";
}

constexpr const char* kMake =
    "public static async method Make(): Task<IShape> return new Sq(); "
    "end Make; ";

void expectAcceptsAndPrints240(const std::string& src) {
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("240\n"));
}

}  // namespace

// ---- REJECT: the unbound escaping await at an interface result ----------

BEZEL_TEST(async_unbound_await_interface_result_5723,
          reject_unbound_var_await_task_iface_30_5) {
    auto r = runAot(rejectProgram(kMake,
        "var r := await Make(); return r.Area();"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kPatternAOwningLocation));
}

BEZEL_TEST(async_unbound_await_interface_result_5723,
          reject_unbound_var_await_task_nullable_iface_30_5) {
    auto r = runAot(rejectProgram(
        "public static async method Make(): Task<IShape?> return new Sq(); "
        "end Make; ",
        "var r := await Make(); return r!.Area();"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kPatternAOwningLocation));
}

BEZEL_TEST(async_unbound_await_interface_result_5723,
          reject_unbound_var_await_generic_task_at_iface_30_5) {
    auto r = runAot(rejectProgram(
        "public static async method MakeG<T>(v: owned T): Task<T> return v; "
        "end MakeG; ",
        "var r := await MakeG<IShape>(new Sq()); return r.Area();"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kPatternAOwningLocation));
}

// §30.3 / §30.5: `.Result` is the Pattern A sibling of `await` and takes the
// same owning-location rule. MEASURED pre-fix: `var r := Make().Result;` at
// `Task<IShape>` was accepted and ASan reported heap-use-after-free.
BEZEL_TEST(async_unbound_await_interface_result_5723,
          reject_unbound_result_read_task_iface_30_5) {
    auto r = runAot(std::string(kShapes) +
        "public class Program "
        "  public static async method Make(): Task<IShape> return new Sq(); "
        "  end Make; "
        "  public static method Main(): int "
        "    var r := Make().Result; return r.Area(); end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r,
        "reading `.Result` on a class-typed `Task<T>` (Pattern A) requires the "
        "task expression to occupy a syntactically-visible owning location"));
}

// Pattern B: the unbound `var` must discharge an owned local, as for a class.
BEZEL_TEST(async_unbound_await_interface_result_5723,
          reject_unbound_var_await_owningtask_iface_30_5) {
    auto r = runAot(rejectProgram(
        "public static async method Make(): OwningTask<IShape> "
        "return new Sq(); end Make; ",
        "var r := await Make(); return r.Area();"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kDroppedWithoutDisposal));
}

// ---- ACCEPT + RUN: the forms the spec admits stay clean ------------------

BEZEL_TEST(async_unbound_await_interface_result_5723,
          accept_bound_await_task_iface_looped_30_5) {
    expectAcceptsAndPrints240(program(kMake,
        "using var t := Make(); var r := await t; sum := sum + r.Area();"));
}

BEZEL_TEST(async_unbound_await_interface_result_5723,
          accept_bound_await_task_nullable_iface_looped_30_5) {
    expectAcceptsAndPrints240(program(
        "public static async method Make(): Task<IShape?> return new Sq(); "
        "end Make; ",
        "using var t := Make(); var r := await t; sum := sum + r!.Area();"));
}

BEZEL_TEST(async_unbound_await_interface_result_5723,
          accept_bound_await_generic_task_at_iface_looped_30_5) {
    expectAcceptsAndPrints240(program(
        "public static async method MakeG<T>(v: owned T): Task<T> return v; "
        "end MakeG; ",
        "using var t := MakeG<IShape>(new Sq()); var r := await t; "
        "sum := sum + r.Area();"));
}

// §30.5: `Process(await Producer())` is admitted - the borrow is confined to
// the full expression.
BEZEL_TEST(async_unbound_await_interface_result_5723,
          accept_await_as_argument_task_iface_looped_30_5) {
    expectAcceptsAndPrints240(program(
        std::string(kMake) +
        "public static method Use(s: IShape): int return s.Area(); end Use; ",
        "sum := sum + Use(await Make());"));
}

// §30.5 kind 2: `return await <owned task>` is the chained slot.
BEZEL_TEST(async_unbound_await_interface_result_5723,
          accept_return_await_chained_task_iface_looped_30_5) {
    expectAcceptsAndPrints240(program(
        std::string(kMake) +
        "public static async method Fwd(): Task<IShape> "
        "using var t := Make(); return await t; end Fwd; ",
        "using var t := Fwd(); var r := await t; sum := sum + r.Area();"));
}

// Pattern B discard: `_ := await ...` disposes the result at statement end.
BEZEL_TEST(async_unbound_await_interface_result_5723,
          accept_discard_await_owningtask_iface_looped_30_5) {
    expectAcceptsAndPrints240(program(
        "public static async method Make(): OwningTask<IShape> "
        "return new Sq(); end Make; ",
        "_ := await Make(); sum := sum + 4;"));
}

// Control: a value result may still be awaited unbound (no obligation).
BEZEL_TEST(async_unbound_await_interface_result_5723,
          accept_unbound_await_value_results_unchanged_30_5) {
    expectAcceptsAndPrints240(program(
        "public static async method N(): Task<int> return 4; end N; "
        "public static async method S(): Task<string> return \"abcd\"; end S; ",
        "var n := await N(); var s := await S(); sum := sum + n + s.Length - 4;"));
}
