// Regression tests for issue #5416 (stdlib §4.11, v0.315):
// `Stack<T>`/`OwningStack<T>`/`Queue<T>`/`OwningQueue<T>` accept `.Length`
// but neither declares it — stdlib §4.11 exposes `Count` (via
// IReadOnlyCollection<T>) and §4.4's Count-vs-Length convention classifies
// resizable collections to Count. The fix removes the four heads from
// kBuiltinIntLengthPropertyHeads (BuiltinRegistry.cpp) and adds explicit
// "no member `Length`" diagnostics per type (ExprTypecheck_Calls.cpp).
//
// Confirmed MEASURED on r23-c28 (origin/main at lane start):
//   var s := new Stack<int>(); s.Push(1); return s.Length;
// compiled at exit 0 with no diagnostics.

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

bool hasError(const std::string& program, const char* keyword) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
                d.message.find(keyword) != std::string::npos)
            return true;
    }
    return false;
}

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// fix #5416 (§4.11, v0.315): `.Length` rejected on Stack<T>
BEZEL_TEST(stack_queue_length_rejected_5416, stack_length_rejected_stdlib_4_11) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var s := new Stack<int>(); "
        "  s.Push(1); "
        "  return s.Length; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program,
        "no member `Length` on `Stack<T>`: use `Count` (stdlib \\S4.11)"));
}

// fix #5416 (§4.11, v0.315): `.Length` rejected on OwningStack<T>
BEZEL_TEST(stack_queue_length_rejected_5416, owningstack_length_rejected_stdlib_4_11) {
    const std::string program =
        "module M; "
        "class Res public Tag: int; public constructor(t: int) Tag := t; end constructor; end Res; "
        "class C public static method Main(): int "
        "  var s := new OwningStack<Res>(); "
        "  s.Push(new Res(1)); "
        "  return s.Length; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program,
        "no member `Length` on `OwningStack<T>`: use `Count` (stdlib \\S4.11)"));
}

// fix #5416 (§4.11, v0.315): `.Length` rejected on Queue<T>
BEZEL_TEST(stack_queue_length_rejected_5416, queue_length_rejected_stdlib_4_11) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var q := new Queue<int>(); "
        "  q.Enqueue(1); "
        "  return q.Length; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program,
        "no member `Length` on `Queue<T>`: use `Count` (stdlib \\S4.11)"));
}

// fix #5416 (§4.11, v0.315): `.Length` rejected on OwningQueue<T>
BEZEL_TEST(stack_queue_length_rejected_5416, owningqueue_length_rejected_stdlib_4_11) {
    const std::string program =
        "module M; "
        "class Res public Tag: int; public constructor(t: int) Tag := t; end constructor; end Res; "
        "class C public static method Main(): int "
        "  var q := new OwningQueue<Res>(); "
        "  q.Enqueue(new Res(1)); "
        "  return q.Length; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program,
        "no member `Length` on `OwningQueue<T>`: use `Count` (stdlib \\S4.11)"));
}

// fix #5416 (§4.11, v0.315): `.Count` (the declared member) still works on Stack<T>
BEZEL_TEST(stack_queue_length_rejected_5416, stack_count_still_works) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var s := new Stack<int>(); "
        "  s.Push(7); s.Push(8); s.Push(9); "
        "  return s.Count; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 3);
}

// fix #5416 (§4.11, v0.315): `.Count` (the declared member) still works on Queue<T>
BEZEL_TEST(stack_queue_length_rejected_5416, queue_count_still_works) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var q := new Queue<int>(); "
        "  q.Enqueue(7); q.Enqueue(8); q.Enqueue(9); "
        "  return q.Count; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 3);
}
