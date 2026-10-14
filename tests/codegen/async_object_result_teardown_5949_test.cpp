// Codegen / soundness - issue #5949 (spec §30.5 and §28.9, v0.338): the
// Pattern A row reads "`Task<T>` for class or interface T (`Object`, a class
// (§3.1), included ...) ... The result's lifetime is tied to the task -
// disposing the task disposes the result", and kind 1 of "Pattern A result
// slots": "the task **owns the result** and its destructor disposes it
// directly". `Object` is a Primitive-kind TypeRef with no declaration, so
// `elementPolicyFor` had no row for it and `patternAResultDisposerFor`
// attached no disposer: a `new Object()` produced by an `async ... :
// Task<Object>` leaked through EVERY accepted consumption form, including the
// canonical bound form `using var t := Make(); var r := await t;`.
//
// Each row loops 60x under the always-ASan `runAotChecked` oracle (LeakSanitizer
// turns a one-shot leak into a failing exit code), asserts the program ran, its
// stdout and exit code. Companion to async_object_result_pattern_5949_test.cpp
// (the reject rows) and async_unbound_await_interface_result_5723_test.cpp.
//
// Follow-up (same issue, and narrow #5704): the first cut attached the
// `Object` result disposer to EVERY non-`await` return under `Task<Object>`,
// so the teardown reclaimed (or read) an object the task never owned -
// `return p;` over the caller's `Object`, `return p as Object;` over a binding
// the frame disposes itself: heap-use-after-free. The second half of this file
// pins the repair: looped rows showing that an operand the task cannot be
// shown to own is left alone. Whether such an operand is to be REJECTED
// (§30.5 *No fourth kind*) is spec-question #6050 and not asserted here.
//
// Not asserted here, filed separately: OwningTask<Object> results that are
// never awaited / read through .Result / discarded (#5979), a user-class
// instance upcast to Object (#5980), a returned Object local with a throw
// before the return (#5981).
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

std::string src(const std::string& members, const std::string& mainSig,
                const std::string& body) {
    return "module M; public class Program " + members +
           "  public static " + mainSig + " " + body + " end Main; "
           "end Program;";
}

// 60 iterations, one `n := n + 4` per iteration: stdout "240".
std::string loopAsync(const std::string& step) {
    return "var i: int := 0; var n: int := 0; "
           "while i < 60 do " + step + " i := i + 1; end while; "
           "Console.WriteLine(n); return 0;";
}

constexpr const char* kTaskMake =
    "public static async method Make(): Task<Object> "
    "return new Object(); end Make; ";
constexpr const char* kTaskQMake =
    "public static async method Make(): Task<Object?> "
    "return new Object(); end Make; ";

constexpr const char* kPlain =
    "public class Plain public x: int := 1; end Plain; ";

std::string withTypes(const std::string& types, const std::string& members,
                      const std::string& mainSig, const std::string& body) {
    return "module M; " + types + "public class Program " + members +
           "  public static " + mainSig + " " + body + " end Main; "
           "end Program;";
}

void expectCleanRun(const std::string& program) {
    JitResult r = runAotChecked(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("240\n"));
}

}  // namespace

// §30.5 canonical form: a `using var` Task<T> local, awaited.
BEZEL_TEST(async_object_result_teardown_5949,
          task_object_bound_using_var_await_looped_30_5) {
    expectCleanRun(src(kTaskMake, "async method Main(): Task<int>",
        loopAsync("do using var t := Make(); var r := await t; "
                  "n := n + 4; end;")));
}

// §30.5 kind 2: `return await` over an owned Pattern A source chains the
// source task into the outer task; the object is disposed exactly once.
BEZEL_TEST(async_object_result_teardown_5949,
          task_object_return_await_chain_looped_30_5) {
    expectCleanRun(src(
        std::string(kTaskMake) +
            "public static async method Fwd(): Task<Object> "
            "return await Make(); end Fwd; ",
        "async method Main(): Task<int>",
        loopAsync("do using var t := Fwd(); var r := await t; "
                  "n := n + 4; end;")));
}

// §30.5 "Disposing an unconsumed ... task": the result is disposed with the
// task although nothing ever awaited it.
BEZEL_TEST(async_object_result_teardown_5949,
          task_object_disposed_without_await_looped_30_5) {
    expectCleanRun(src(kTaskMake, "method Main(): int",
        loopAsync("do using var t := Make(); n := n + 4; end;")));
}

// §30.5: `.Result` reads a borrow; the task still owns and disposes it.
BEZEL_TEST(async_object_result_teardown_5949,
          task_object_result_read_looped_30_5) {
    expectCleanRun(src(kTaskMake, "method Main(): int",
        loopAsync("do using var t := Make(); var r := t.Result; "
                  "n := n + 4; end;")));
}

// §30.5 consumer rules: an unbound owned temporary operand of `_ := await`.
BEZEL_TEST(async_object_result_teardown_5949,
          task_object_discard_await_looped_30_5) {
    expectCleanRun(src(kTaskMake, "async method Main(): Task<int>",
        loopAsync("_ := await Make(); n := n + 4;")));
}

// §30.5 kind 1: "a named owned binding of the body ... consumed marker-free at
// the return" - the task owns it.
BEZEL_TEST(async_object_result_teardown_5949,
          task_object_return_named_owned_local_looped_30_5) {
    expectCleanRun(src(
        "public static async method Make(): Task<Object> "
        "var o: Object := new Object(); return o; end Make; ",
        "async method Main(): Task<int>",
        loopAsync("do using var t := Make(); var r := await t; "
                  "n := n + 4; end;")));
}

// A nullable T (`Task<Object?>`) with a non-null result.
BEZEL_TEST(async_object_result_teardown_5949,
          task_nullable_object_bound_await_looped_30_5) {
    expectCleanRun(src(kTaskQMake, "async method Main(): Task<int>",
        loopAsync("do using var t := Make(); var r := await t; "
                  "n := n + 4; end;")));
}

// kind 1's null clause: `null` under a nullable T has nothing to dispose.
BEZEL_TEST(async_object_result_teardown_5949,
          task_nullable_object_null_result_looped_30_5) {
    expectCleanRun(src(
        "public static async method Make(): Task<Object?> "
        "return null; end Make; ",
        "async method Main(): Task<int>",
        loopAsync("do using var t := Make(); var r := await t; "
                  "n := n + 4; end;")));
}

// §30.5 kind 1 / §28.9: "a runtime-managed operand returned there - a box, a
// `string` - is kind 1 with nothing for the teardown to dispose". A string
// literal under Task<Object> must not be reclaimed by the Object thunk (no
// double free, no free of a static literal).
BEZEL_TEST(async_object_result_teardown_5949,
          task_object_string_literal_result_untouched_28_9) {
    expectCleanRun(src(
        "public static async method Make(): Task<Object> "
        "return \"abcd\"; end Make; ",
        "async method Main(): Task<int>",
        loopAsync("do using var t := Make(); var r := await t; "
                  "n := n + 4; end;")));
}

// §30.5 kind 1 / §28.9: a value-typed operand is not an Object instance; the
// teardown must not reclaim (or even dereference) it. Never awaited, so only
// the task's teardown touches the result slot.
BEZEL_TEST(async_object_result_teardown_5949,
          task_object_value_typed_result_not_dereferenced_28_9) {
    expectCleanRun(src(
        "public static async method Make(): Task<Object> "
        "return 42; end Make; ",
        "method Main(): int",
        loopAsync("do using var t := Make(); n := n + 4; end;")));
}

// ---------------------------------------------------------------------------
// Follow-up rows: operands the task does not own.
// ---------------------------------------------------------------------------

// A plain `Object` parameter is accepted here (whether §30.5 *No fourth kind*
// reaches a location that may hold a box is #6050). What IS pinned: the task
// does not reclaim it. The first row is the review's repro, where the first
// cut freed the caller's `keep`.
BEZEL_TEST(async_object_result_teardown_5949,
          task_object_plain_param_not_reclaimed_by_task_looped_30_5) {
    expectCleanRun(src(
        "public static async method Echo(p: Object): Task<Object> "
        "return p; end Echo; ",
        "async method Main(): Task<int>",
        loopAsync("using var keep := new Object(); "
                  "do using var t := Echo(keep); var r := await t; end; "
                  "n := n + 4 + keep.GetHashCode() * 0;")));
}

// Two tasks over the same caller-owned object.
BEZEL_TEST(async_object_result_teardown_5949,
          task_object_plain_param_two_tasks_not_reclaimed_looped_30_5) {
    expectCleanRun(src(
        "public static async method Echo(p: Object): Task<Object> "
        "return p; end Echo; ",
        "async method Main(): Task<int>",
        loopAsync("using var keep := new Object(); "
                  "do using var t := Echo(keep); using var u := Echo(keep); "
                  "var r := await t; var q := await u; end; "
                  "n := n + 4 + keep.GetHashCode() * 0;")));
}

// ---------------------------------------------------------------------------
// Accept rows. §30.5: "a runtime-managed operand - a `string`, a box under
// `Task<Object>` - is no borrow and is not reached (§28.3 item 5)"; and an
// operand the task cannot be shown to own gets NO disposer.
//
// The upcast rows below (`return p as Object` over an owned parameter or an
// owned local, and the collection held through an `Object` local) pin only
// that the TASK does not free what the body's frame already disposed. The
// result pointer the awaiter receives there is dangling and these programs
// never read through it; that is the separate, pre-existing #6051 (`return
// <owned binding> as Object` leaves the result dangling), not asserted here.
// ---------------------------------------------------------------------------

// §28.3 Producers: `x as T` borrows `x` and "`x` retains its ownership state" -
// the frame still disposes its `owned` parameter itself, so the task must not
// touch the result slot at teardown (it read the freed block).
BEZEL_TEST(async_object_result_teardown_5949,
          task_object_owned_param_upcast_not_reclaimed_by_task_looped_30_5) {
    expectCleanRun(withTypes(kPlain,
        "public static async method F(owned p: Plain): Task<Object> "
        "return p as Object; end F; ",
        "async method Main(): Task<int>",
        loopAsync("do using var t := F(new Plain()); var r := await t; "
                  "n := n + 4; end;")));
}

// The same shape with two live tasks, and one never awaited.
BEZEL_TEST(async_object_result_teardown_5949,
          task_object_owned_param_upcast_two_tasks_looped_30_5) {
    expectCleanRun(withTypes(kPlain,
        "public static async method F(owned p: Plain): Task<Object> "
        "return p as Object; end F; ",
        "async method Main(): Task<int>",
        loopAsync("do using var t := F(new Plain()); "
                  "using var u := F(new Plain()); var r := await t; "
                  "n := n + 4; end;")));
}

// An owned LOCAL upcast with `as`: the local keeps its obligation.
BEZEL_TEST(async_object_result_teardown_5949,
          task_object_owned_local_upcast_not_reclaimed_by_task_looped_30_5) {
    expectCleanRun(withTypes(kPlain,
        "public static async method F(): Task<Object> "
        "var p := new Plain(); return p as Object; end F; ",
        "async method Main(): Task<int>",
        loopAsync("do using var t := F(); var r := await t; "
                  "n := n + 4; end;")));
}

// A collection the body owns, held through an `Object` local: disposed by the
// body at scope end, never by the task.
BEZEL_TEST(async_object_result_teardown_5949,
          task_object_array_through_object_local_not_reclaimed_looped_30_5) {
    expectCleanRun(src(
        "public static async method F(): Task<Object> "
        "var a := new Array<int>(3); var b: Object := a; return b; end F; ",
        "async method Main(): Task<int>",
        loopAsync("do using var t := F(); var r := await t; "
                  "n := n + 4; end;")));
}

// K1.6: a function value is runtime-managed; upcast into an `Object` local.
BEZEL_TEST(async_object_result_teardown_5949,
          task_object_function_value_through_object_local_looped_30_5) {
    expectCleanRun(src(
        "public static async method F(): Task<Object> "
        "var f := lambda() => 1; var b: Object := f as Object; return b; "
        "end F; ",
        "async method Main(): Task<int>",
        loopAsync("do using var t := F(); var r := await t; "
                  "n := n + 4; end;")));
}

// A value-typed FIELD read is boxed at the return - "no borrow and is not
// reached" - so the field-read rejection must not fire on it.
BEZEL_TEST(async_object_result_teardown_5949,
          accept_value_typed_field_read_boxed_at_return_looped_30_5) {
    expectCleanRun(withTypes(kPlain,
        "public static async method F(p: Plain): Task<Object> "
        "return p.x; end F; ",
        "async method Main(): Task<int>",
        loopAsync("do using var o := new Plain(); using var t := F(o); "
                  "var r := await t; n := n + 4; end;")));
}

// ... nor on a value-typed indexer result.
BEZEL_TEST(async_object_result_teardown_5949,
          accept_value_typed_indexer_result_boxed_at_return_looped_30_5) {
    expectCleanRun(src(
        "public static async method F(a: Array<int>): Task<Object> "
        "return a[0]; end F; ",
        "async method Main(): Task<int>",
        loopAsync("do var a: Array<int> := [1, 2]; using var t := F(a); "
                  "var r := await t; n := n + 4; end;")));
}

// A `string` / value-typed plain PARAMETER is a runtime-managed operand, not
// "a plain parameter" borrow.
BEZEL_TEST(async_object_result_teardown_5949,
          accept_string_and_int_param_boxed_at_return_looped_30_5) {
    expectCleanRun(src(
        "public static async method S(s: string): Task<Object> "
        "return s; end S; "
        "public static async method I(k: int): Task<Object> "
        "return k; end I; ",
        "async method Main(): Task<int>",
        loopAsync("do using var t := S(\"ab\"); using var u := I(3); "
                  "n := n + 4; end;")));
}

// A call RESULT on a plain `Object` parameter is not a read of the parameter.
BEZEL_TEST(async_object_result_teardown_5949,
          accept_string_call_result_on_plain_object_param_looped_30_5) {
    expectCleanRun(src(
        "public static async method F(p: Object): Task<Object> "
        "return p.ToString(); end F; ",
        "async method Main(): Task<int>",
        loopAsync("do using var o := new Object(); using var t := F(o); "
                  "var r := await t; n := n + 4; end;")));
}

// kind 1 still holds where the body DOES own the value: a nullable owned
// local returned through `!`, and an owned local chosen on one of two paths.
BEZEL_TEST(async_object_result_teardown_5949,
          task_object_owned_local_bang_and_branch_looped_30_5) {
    expectCleanRun(src(
        "public static async method A(): Task<Object> "
        "var o: Object? := new Object(); return o!; end A; "
        "public static async method B(c: bool): Task<Object?> "
        "if c then var o := new Object(); return o; end if; "
        "return null; end B; ",
        "async method Main(): Task<int>",
        loopAsync("do using var t := A(); var r := await t; "
                  "using var u := B(i mod 2 = 0); var q := await u; "
                  "n := n + 4; end;")));
}

// The async-lambda forms keep the disposer for a literal `new Object()`.
BEZEL_TEST(async_object_result_teardown_5949,
          task_object_async_lambda_fresh_object_looped_30_5) {
    expectCleanRun(src("", "async method Main(): Task<int>",
        loopAsync("do var f := async lambda(): Task<Object> => "
                  "do return new Object(); end; "
                  "using var t := f(); var r := await t; n := n + 4; end;")));
}

// ---------------------------------------------------------------------------
// Review rows (multi-`return` bodies). An owned `Object` local declared
// OUTSIDE the returning block is still owned at another exit, so it carries
// the §28.3 scope-end auto-drop; §30.5 kind 1: "a named owned binding of the
// body, a plain owned local ..., consumed marker-free at the return ...: the
// task owns the result and its destructor disposes it directly". The frame
// freed the returned local and the task's teardown reclaimed it again
// (heap-use-after-free in the `Object` result disposer; the class/interface
// form is #6052). Each row loops under ASan, alternating the path taken.
// ---------------------------------------------------------------------------

std::string twoPathBody(const std::string& body) {
    return src(
        "public static async method F(c: bool): Task<Object> " + body +
            " end F; ",
        "async method Main(): Task<int>",
        loopAsync("do using var t := F(i mod 2 = 0); var r := await t; "
                  "n := n + 4; end;"));
}

// Two owned locals, one returned on each path.
BEZEL_TEST(async_object_result_teardown_5949,
          task_object_two_owned_locals_one_per_path_looped_30_5) {
    expectCleanRun(twoPathBody(
        "var o: Object := new Object(); var q: Object := new Object(); "
        "if c then return q; end if; return o;"));
}

// Early return of the outer local, fresh value on the fall-through path.
BEZEL_TEST(async_object_result_teardown_5949,
          task_object_early_return_outer_local_else_fresh_looped_30_5) {
    expectCleanRun(twoPathBody(
        "var o: Object := new Object(); if c then return o; end if; "
        "return new Object();"));
}

// Early return of the outer local, a second local declared after it.
BEZEL_TEST(async_object_result_teardown_5949,
          task_object_early_return_outer_local_then_later_local_looped_30_5) {
    expectCleanRun(twoPathBody(
        "var o: Object := new Object(); if c then return o; end if; "
        "var q: Object := new Object(); return q;"));
}

// Fresh value on the early path, the outer local on the fall-through path.
BEZEL_TEST(async_object_result_teardown_5949,
          task_object_early_fresh_else_outer_local_looped_30_5) {
    expectCleanRun(twoPathBody(
        "var o: Object := new Object(); if c then return new Object(); "
        "end if; return o;"));
}

// The outer local returned from inside a loop body.
BEZEL_TEST(async_object_result_teardown_5949,
          task_object_outer_local_returned_inside_while_looped_30_5) {
    expectCleanRun(twoPathBody(
        "var o: Object := new Object(); var k := 0; "
        "while k < 3 do k := k + 1; if k = 2 then return o; end if; "
        "end while; return new Object();"));
}

// The same through a `try`-`finally`: the return is routed through the
// finally's exit dispatch, which disposes the enclosing scopes.
BEZEL_TEST(async_object_result_teardown_5949,
          task_object_outer_local_returned_through_finally_looped_30_5) {
    expectCleanRun(twoPathBody(
        "var o: Object := new Object(); var k := 0; "
        "try if c then return o; end if; finally k := k + 1; end try; "
        "return new Object();"));
}
