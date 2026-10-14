// Ownership - fix #6101 (spec §30.5, §28.3, v0.338): `var r := await t;` at
// `Task<Object>` / `Task<Object?>` is a borrow of `t`, exactly as it is for a
// class result.
//
//   §30.5 Producer-side rules - "`Task<T>` for class or interface T (`Object`,
//     a class (§3.1), included ...): the task may be awaited from multiple
//     places. Every awaiter receives a borrow of the same underlying result.
//     The result's lifetime is tied to the task - disposing the task disposes
//     the result."
//   §30.5 Consumer-side rules - "`var r := await task;` - `r` is a non-owned
//     local with the same source as `task`."
//   §28.3 Borrow lifetime rules - "A borrow may not be assigned to a local
//     whose scope strictly encloses the borrow's source's scope".
//
// THE DEFECT. `borrowSourceForExpr`'s await arm recorded the source on `r`
// whatever the result type, but every ONWARD read of `r` goes through Shape 1,
// which is gated on `returnedExprMayBorrow`; a plain-`Object` holder is a
// Primitive-kind TypeRef that the gate reads as a value copy. So each store of
// `r` past the task was accepted at `Task<Object>` where the byte-identical
// class program is rejected, and the stored reference outlived the result.
//
// Every reject row asserts the specific diagnostic; the class twin of the
// first shape is pinned beside it so the two cannot drift apart again.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

constexpr const char* kHeldByKeep =
    "borrow held by `keep` would outlive its source `t`";
constexpr const char* kStoredIntoField =
    "borrow of `t` stored into a field would outlive its source";
constexpr const char* kStoredIntoSlot =
    "borrow of `t` stored into `a[...]` would outlive its source";

// `resultType` is the producer's `Task<...>` argument; `make` the expression
// it returns. `pre` runs at the top of each iteration, `inner` inside the
// block that owns the task, `post` after that block has disposed it.
std::string program(const std::string& resultType, const std::string& make,
                    const std::string& pre, const std::string& inner,
                    const std::string& post) {
    return "module M; "
           "public class Plain public x: int := 1; end Plain; "
           "public class Holder public f: Object?; end Holder; "
           "public class Program "
           "  public static async method F(): Task<" + resultType + "> "
           "    return " + make + "; "
           "  end F; "
           "  public static async method Main(): Task<int> "
           "    var i: int := 0; var n: int := 0; "
           "    while i < 30 do "
           "      " + pre + " "
           "      do using var t := F(); var r := await t; " + inner + " end; "
           "      " + post + " "
           "      i := i + 1; "
           "    end while; "
           "    Console.WriteLine(n); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

void expectRejected(const JitResult& r, const char* needle) {
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle));
}

void expectRuns30(const JitResult& r) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("30\n"));
}

}  // namespace

// The issue's repro: the result stored into a local of the enclosing scope and
// read after the task's block.
BEZEL_TEST(async_object_await_borrow_escape_6101,
          reject_object_result_stored_into_outer_local_30_5) {
    expectRejected(
        runAot(program("Object", "new Object()", "var keep: Object? := null;",
                       "keep := r;",
                       "n := n + keep!.GetHashCode() * 0 + 1;")),
        kHeldByKeep);
}

// The class twin: rejected before the fix and still, with the same text.
BEZEL_TEST(async_object_await_borrow_escape_6101,
          control_class_result_stored_into_outer_local_rejected_30_5) {
    expectRejected(
        runAot(program("Plain", "new Plain()", "var keep: Plain? := null;",
                       "keep := r;", "n := n + keep!.x;")),
        kHeldByKeep);
}

// One more hop: `b` re-borrows `r` and inherits its source.
BEZEL_TEST(async_object_await_borrow_escape_6101,
          reject_object_result_alias_stored_into_outer_local_28_3) {
    expectRejected(
        runAot(program("Object", "new Object()", "var keep: Object? := null;",
                       "var b := r; keep := b;",
                       "n := n + keep!.GetHashCode() * 0 + 1;")),
        kHeldByKeep);
}

BEZEL_TEST(async_object_await_borrow_escape_6101,
          reject_object_result_stored_into_field_28_3) {
    expectRejected(
        runAot(program("Object", "new Object()", "using var h := new Holder();",
                       "h.f := r;",
                       "n := n + h.f!.GetHashCode() * 0 + 1;")),
        kStoredIntoField);
}

BEZEL_TEST(async_object_await_borrow_escape_6101,
          reject_object_result_stored_into_array_slot_28_3) {
    expectRejected(
        runAot(program("Object", "new Object()",
                       "var a: Array<Object?> := [null];", "a[0] := r;",
                       "n := n + a[0]!.GetHashCode() * 0 + 1;")),
        kStoredIntoSlot);
}

// `Task<Object?>`: the nullable result is the same borrow.
BEZEL_TEST(async_object_await_borrow_escape_6101,
          reject_nullable_object_result_stored_into_outer_local_30_5) {
    expectRejected(
        runAot(program("Object?", "new Object()", "var keep: Object? := null;",
                       "keep := r;",
                       "n := n + keep!.GetHashCode() * 0 + 1;")),
        kHeldByKeep);
}

// `r!` is not a new source (§17.8).
BEZEL_TEST(async_object_await_borrow_escape_6101,
          reject_nullable_object_result_bang_stored_into_outer_local_17_8) {
    expectRejected(
        runAot(program("Object?", "new Object()", "var keep: Object? := null;",
                       "keep := r!;",
                       "n := n + keep!.GetHashCode() * 0 + 1;")),
        kHeldByKeep);
}

// The rule is read off the static type, so a runtime-managed operand returned
// at `Task<Object>` (§30.5: "a box, a `string`") is the same borrow.
BEZEL_TEST(async_object_await_borrow_escape_6101,
          reject_string_in_object_result_stored_into_outer_local_30_5) {
    expectRejected(
        runAot(program("Object", "\"lit\"", "var keep: Object? := null;",
                       "keep := r;", "n := n + 1;")),
        kHeldByKeep);
}

// Accepted: the borrow is read, and re-borrowed, inside the task's scope. On
// runAotChecked since the task disposes its `Object` result (narrow #5949).
BEZEL_TEST(async_object_await_borrow_escape_6101,
          accept_object_result_read_inside_task_scope_30_5) {
    expectRuns30(runAotChecked(program("Object", "new Object()", "",
                                "n := n + r.GetHashCode() * 0 + 1;", "")));
}

BEZEL_TEST(async_object_await_borrow_escape_6101,
          accept_object_result_alias_read_inside_task_scope_28_3) {
    expectRuns30(
        runAotChecked(program("Object", "new Object()", "",
                       "var b := r; n := n + b.ToString().Length * 0 + 1;",
                       "")));
}

// Accepted: an inner-scope holder, narrower than the task, takes the borrow.
BEZEL_TEST(async_object_await_borrow_escape_6101,
          accept_object_result_stored_into_inner_local_28_3) {
    expectRuns30(
        runAotChecked(program("Object", "new Object()", "",
                       "var keep: Object? := null; keep := r; "
                       "n := n + keep!.GetHashCode() * 0 + 1;",
                       "")));
}
