// Codegen — fix #4750 / #4772, pin #4771 (§29.1, v0.307; stdlib §4.13): the
// `TakeCurrent()` guard of a builtin drain view's owning enumerator.
//
// §29.1 / stdlib §4.13: "A second take without an intervening advance, or a
// take before the first advance/after exhaustion, raises
// InvalidOperationException." The builtin drain frames
// (`wrapArrayAsIOwningEnumerator`, forward and LIFO, and
// `wrapLinkedListAsIOwningEnumerator`) instead returned null — "null in, null
// out" — so the `catch` arm never ran. MEASURED on the pre-fix tree
// (r20-c25): OwningArray / OwningQueue / OwningSet print "no-raise";
// OwningStack / OwningLinkedList SIGSEGV earlier, at the unset view slot
// (pinned separately by ioe_slot_init_4750_test.cpp).
//
// Every head of the drain family (stdlib §4.5 / §4.7 / §4.10 / §4.11) is
// pinned for each of the three raising shapes. Oracle: stdout — "caught" is
// printed only by the IOE arm, and "no-raise" by the fall-through.
// Conformance rows l4_13_0001 / l4_13_0002 are the OwningArray instances.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// (Never put a C++ `//` comment inside these literals: it is Bezel source.)
std::string program(const char* head, const char* add, const char* body) {
    return std::string(
               "module M4750T; "
               "class W "
               "  public Id: int; "
               "  public constructor(id: int) Id := id; end constructor; "
               "end W; "
               "class C "
               "  public static method Main(): int "
               "    using var pool := new ") + head + "<W>(); "
               "    pool." + add + "(new W(7)); "
               "    using var view := pool.Drain(); "
               "    using var it := view.GetOwningEnumerator(); " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// A second take without an intervening advance.
const char* kSecondTake =
    "    _ := it.MoveNext(); "
    "    using var w := it.TakeCurrent(); "
    "    Console.WriteLine(w.Id); "
    "    try "
    "      _ := it.TakeCurrent(); "
    "      Console.WriteLine(\"no-raise\"); "
    "    catch "
    "      InvalidOperationException => Console.WriteLine(\"caught\") "
    "    end; ";

// A take before the first advance.
const char* kBeforeAdvance =
    "    try "
    "      _ := it.TakeCurrent(); "
    "      Console.WriteLine(\"no-raise\"); "
    "    catch "
    "      InvalidOperationException => Console.WriteLine(\"caught\") "
    "    end; "
    "    Console.WriteLine(pool.Count); ";

// A take after exhaustion (the one element taken, then MoveNext is false).
const char* kAfterExhaustion =
    "    _ := it.MoveNext(); "
    "    using var w := it.TakeCurrent(); "
    "    if it.MoveNext() then Console.WriteLine(\"more\"); end; "
    "    try "
    "      _ := it.TakeCurrent(); "
    "      Console.WriteLine(\"no-raise\"); "
    "    catch "
    "      InvalidOperationException => Console.WriteLine(\"caught\") "
    "    end; "
    "    Console.WriteLine(w.Id); ";

void expectOut(const char* head, const char* add, const char* body,
               const char* want) {
    JitResult r = runAot(program(head, add, body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(want));
}

struct Head { const char* name; const char* add; };
const Head kHeads[] = {{"OwningArray", "Add"},
                       {"OwningQueue", "Enqueue"},
                       {"OwningSet", "Add"},
                       {"OwningStack", "Push"},
                       {"OwningLinkedList", "AddLast"}};

}  // namespace

// per §29.1 / stdlib §4.13: second take without an advance raises IOE.
BEZEL_TEST(IOwningEnumerableTakeCurrentIOE_4750, owning_array_second_take_raises) {
    expectOut("OwningArray", "Add", kSecondTake, "7\ncaught\n");
}
BEZEL_TEST(IOwningEnumerableTakeCurrentIOE_4750, owning_queue_second_take_raises) {
    expectOut("OwningQueue", "Enqueue", kSecondTake, "7\ncaught\n");
}
BEZEL_TEST(IOwningEnumerableTakeCurrentIOE_4750, owning_set_second_take_raises) {
    expectOut("OwningSet", "Add", kSecondTake, "7\ncaught\n");
}
BEZEL_TEST(IOwningEnumerableTakeCurrentIOE_4750, owning_stack_second_take_raises) {
    expectOut("OwningStack", "Push", kSecondTake, "7\ncaught\n");
}
BEZEL_TEST(IOwningEnumerableTakeCurrentIOE_4750, owning_linked_list_second_take_raises) {
    expectOut("OwningLinkedList", "AddLast", kSecondTake, "7\ncaught\n");
}

// per §29.1 / stdlib §4.13: a take before the first advance raises IOE, and
// the element stays in the collection (Count 1).
BEZEL_TEST(IOwningEnumerableTakeCurrentIOE_4750, every_head_take_before_advance_raises) {
    for (const Head& h : kHeads) expectOut(h.name, h.add, kBeforeAdvance, "caught\n1\n");
}

// per §29.1 / stdlib §4.13: a take after exhaustion raises IOE.
BEZEL_TEST(IOwningEnumerableTakeCurrentIOE_4750, every_head_take_after_exhaustion_raises) {
    for (const Head& h : kHeads) expectOut(h.name, h.add, kAfterExhaustion, "caught\n7\n");
}
