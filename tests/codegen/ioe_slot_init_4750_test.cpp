// Codegen — fix #4750 / #4772, pin #4771 (§29.1 / §29.2 / §28.3; stdlib §4.5 /
// §4.7 / §4.10 / §4.11 / §4.13, v0.307): the NAMED-LOCAL slot of a builtin
// `coll.Drain()` view.
//
//     using var view := pool.Drain();
//     using var it := view.GetOwningEnumerator();
//
// THE DEFECT. `pool.Drain()` lowers to no value as a plain member call — only
// the for-in desugaring and the GetOwningEnumerator dispatch recognise the
// shape, by walking back to the initializer — so `lowerLocalDecl` degraded the
// slot to zero with the #2434 `internal:` note. For OwningArray / OwningQueue /
// OwningSet that was masked: the GetOwningEnumerator dispatch re-derives the
// collection from the initializer. For OwningStack and OwningLinkedList it was
// not: the dispatch dispatched vtable slot 4 on the null slot. MEASURED on the
// pre-fix tree (r20-c25): `second_OwningStack` / `second_OwningLinkedList`
// probes SIGSEGV (rc 139); the three array-forward heads print the right
// element.
//
// THE SOURCE GATE (#4772's question). stdlib §4.13: "every owning collection's
// `Drain()` mints a fresh drain-view frame — a caller-owned enumerable over the
// collection, never the collection re-exposed". The heads that declare an
// element-yielding `Drain()` are the owning collections minus the dictionary
// (whose drain yields `DrainedEntry<K, V>`): OwningArray (§4.5), OwningLinkedList
// (§4.7), OwningSet (§4.10), OwningStack and OwningQueue (§4.11). `OwningBuffer`
// declares no `Drain()` (§4.14). The frame itself is chosen by the §29.2
// coercion authority by BACKING — forward array, LIFO array (§4.11 pop order)
// or node list — so every head is pinned here, with its own order.
//
// ORACLES. Value assertions on stdout (the taken element, the collection's
// residual count, and a destructor count proving each element was disposed
// exactly once — taken one by the consumer, the rest by the collection). The
// absence of the #2434 note is pinned in lit
// (tests/lit/drain_view_named_local_no_degrade_note_4750.bzl), NOT here: the
// AOT harness drops a successful compile's warnings, so a gtest assertion on
// it passed on the pre-fix build (measured). The two `_loop` rows run
// the shape 200 times under `runAotChecked`, so an unclaimed 24-byte frame is
// LSan-visible (a one-shot row can leave it reachable and pass vacuously).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// (Never put a C++ `//` comment inside these literals: it is Bezel source.)
std::string program(const char* head, const char* add) {
    return std::string(
               "module M4750S; "
               "class W "
               "  public static _n: int := 0; "
               "  public Id: int; "
               "  public constructor(id: int) Id := id; end constructor; "
               "  destructor _n := _n + 1; end destructor; "
               "end W; "
               "class C "
               "  public static method Main(): int "
               "    do "
               "      using var pool := new ") + head + "<W>(); "
               "      pool." + add + "(new W(7)); "
               "      pool." + add + "(new W(9)); "
               "      using var view := pool.Drain(); "
               "      using var it := view.GetOwningEnumerator(); "
               "      var got := 0; "
               "      if it.MoveNext() then "
               "        using var w := it.TakeCurrent(); "
               "        got := w.Id; "
               "      end; "
               "      Console.WriteLine($\"got={got} left={pool.Count}\"); "
               "    end; "
               "    Console.WriteLine($\"disposed={W._n}\"); "
               "    return 0; "
               "  end Main; "
               "end C;";
}

void expectSlotInit(const char* head, const char* add, const char* got) {
    JitResult r = runAot(program(head, add));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("got=") + got + " left=1\ndisposed=2\n");
}

}  // namespace

// per stdlib §4.5 / §4.13, §29.1: front-to-back drain, first element 7.
BEZEL_TEST(IOwningEnumerableSlotInit_4750, owning_array_named_view_takes_first_element) {
    expectSlotInit("OwningArray", "Add", "7");
}

// per stdlib §4.11: a queue drains in dequeue order — 7 first.
BEZEL_TEST(IOwningEnumerableSlotInit_4750, owning_queue_named_view_takes_front_element) {
    expectSlotInit("OwningQueue", "Enqueue", "7");
}

// per stdlib §4.11: a stack drains in POP order — the top (9) first. Pre-fix:
// SIGSEGV (slot 4 dispatched on the zero-initialized slot).
BEZEL_TEST(IOwningEnumerableSlotInit_4750, owning_stack_named_view_takes_top_element_lifo) {
    expectSlotInit("OwningStack", "Push", "9");
}

// per stdlib §4.7: head-to-tail — 7 first. Pre-fix: SIGSEGV, as the stack.
BEZEL_TEST(IOwningEnumerableSlotInit_4750, owning_linked_list_named_view_takes_head_element) {
    expectSlotInit("OwningLinkedList", "AddLast", "7");
}

// per stdlib §4.10: OwningSet<T> is `where T: class`, so it is always the
// dense-array backing and drains exactly as the `for var x in s.Drain()` loop
// does (#1475). The NEGATIVE half of #4772: the set is in the drain family and
// must NOT double-free — `disposed=2` is the exact-once count, and the checked
// row below is the ASan half. Order is unspecified by §4.10, so the taken
// element is asserted to be one of the two, not a particular one.
BEZEL_TEST(IOwningEnumerableSlotInit_4750, owning_set_named_view_drains_exactly_once) {
    JitResult r = runAot(program("OwningSet", "Add"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText == "got=7 left=1\ndisposed=2\n" ||
                      r.stdoutText == "got=9 left=1\ndisposed=2\n");
}

// Every head, loop-amplified (200 iterations) under `runAotChecked`: the
// minted view frame is CLAIMED at scope end — pre-claim-fix it leaked 24 B per
// `using var view := pool.Drain();` (MEASURED: the #2146 alias carve declined
// the claim over a slot that no longer holds the receiver) — and nothing is
// disposed twice: the OwningSet row is the #4772 negative case (no
// double-free), and `disposed=400` is the exact-once count (2 per iteration).
BEZEL_TEST(IOwningEnumerableSlotInit_4750, every_head_named_view_frame_claimed_loop) {
    struct Row { const char* head; const char* add; const char* want; };
    const Row rows[] = {
        // got + left per iteration: 7 + 1 (front first), 9 + 1 (stack top).
        {"OwningArray", "Add", "s=1600 disposed=400\n"},
        {"OwningQueue", "Enqueue", "s=1600 disposed=400\n"},
        {"OwningStack", "Push", "s=2000 disposed=400\n"},
        {"OwningLinkedList", "AddLast", "s=1600 disposed=400\n"},
        {"OwningSet", "Add", nullptr},  // §4.10 order unspecified
    };
    for (const Row& row : rows) {
        std::string src = std::string(
            "module M4750L; "
            "class W "
            "  public static _n: int := 0; "
            "  public Id: int; "
            "  public constructor(id: int) Id := id; end constructor; "
            "  destructor _n := _n + 1; end destructor; "
            "end W; "
            "class C "
            "  public static method Once(): int "
            "    using var pool := new ") + row.head + "<W>(); "
            "    pool." + row.add + "(new W(7)); "
            "    pool." + row.add + "(new W(9)); "
            "    using var view := pool.Drain(); "
            "    using var it := view.GetOwningEnumerator(); "
            "    var got := 0; "
            "    if it.MoveNext() then "
            "      using var w := it.TakeCurrent(); "
            "      got := w.Id; "
            "    end; "
            "    return got + pool.Count; "
            "  end Once; "
            "  public static method Main(): int "
            "    var s := 0; "
            "    for var i in 0..<200 do s := s + Once(); end; "
            "    Console.WriteLine($\"s={s} disposed={W._n}\"); "
            "    return 0; "
            "  end Main; "
            "end C;";
        JitResult r = runAotChecked(src);
        BEZEL_EXPECT_TRUE(r.ran);
        BEZEL_EXPECT_EQ(r.exitCode, 0);
            if (row.want) {
            BEZEL_EXPECT_EQ(r.stdoutText, std::string(row.want));
        } else {
            const bool ok = r.stdoutText == "s=1600 disposed=400\n" ||
                            r.stdoutText == "s=2000 disposed=400\n";
            BEZEL_EXPECT_TRUE(ok);
        }
    }
}

// The plain (non-`using`) binding, every head: the minted frame is claimed at
// scope end by `registerIterableSourceDisposal` and the bound-drain for-in
// (`lowerOwnedSeqDrainFor`) still drains through the receiver — no leak, no
// double-free, each element disposed exactly once (loop-amplified, 200).
BEZEL_TEST(IOwningEnumerableSlotInit_4750, every_head_plain_var_view_frame_claimed_loop) {
    struct Row { const char* head; const char* add; };
    const Row rows[] = {{"OwningArray", "Add"}, {"OwningQueue", "Enqueue"},
                        {"OwningStack", "Push"}, {"OwningLinkedList", "AddLast"},
                        {"OwningSet", "Add"}};
    for (const Row& row : rows) {
        std::string src = std::string(
            "module M4750P; "
            "class W "
            "  public static _n: int := 0; "
            "  public Id: int; "
            "  public constructor(id: int) Id := id; end constructor; "
            "  destructor _n := _n + 1; end destructor; "
            "end W; "
            "class C "
            "  public static method Once(): int "
            "    using var pool := new ") + row.head + "<W>(); "
            "    pool." + row.add + "(new W(7)); "
            "    pool." + row.add + "(new W(9)); "
            "    var view := pool.Drain(); "
            "    var n := 0; "
            "    for var x in view do "
            "      using var y := move x; "
            "      n := n + y.Id; "
            "    end; "
            "    return n + pool.Count; "
            "  end Once; "
            "  public static method Main(): int "
            "    var s := 0; "
            "    for var i in 0..<200 do s := s + Once(); end; "
            "    Console.WriteLine($\"s={s} disposed={W._n}\"); "
            "    return 0; "
            "  end Main; "
            "end C;";
        JitResult r = runAotChecked(src);
        BEZEL_EXPECT_TRUE(r.ran);
        BEZEL_EXPECT_EQ(r.exitCode, 0);
        BEZEL_EXPECT_EQ(r.stdoutText, std::string("s=3200 disposed=400\n"));
    }
}
