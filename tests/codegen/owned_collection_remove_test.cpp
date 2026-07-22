// Codegen tests for OwningDictionary<K,V>.Remove(key) and
// OwningLinkedList<T>.Remove(item) disposal correctness.
//
// Three bugs (M1 / M2 / M3):
//
//   M1 (leak §4.9): OwningDictionary<K,V>.Remove(key) freed only the key; the
//      removed owned value's destructor never ran.
//      Fix: add bz_dict_remove_str_disposing / bz_dict_remove_gen_disposing
//      runtime functions that call dispose_val on the value before unlinking;
//      pass owningDictValueDisposer from the codegen (as Clear already does).
//
//   M2 (no-op §4.6/§4.3): OwningLinkedList<T>.Remove(item) had no m == "Remove"
//      case in tryLowerLinkedListInstanceCall → fell through to nullptr → silent
//      no-op. Fix: add the case, using Find + RemoveNode as the pattern.
//
//   M3 (CRASH, consequence of M2): storing lst.Remove(b) result in a var and
//      using it in a conditional produced an i32 branch condition instead of i1,
//      crashing the LLVM IR verifier. Fixing M2 (returning a proper i1 bool) also
//      fixes M3.
//
// Tests derive from stdlib §4.9 (OwningDictionary), §4.6 (LinkedList), §4.3
// (IOwningCollection.Remove).

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Plain AOT run (value only) — for the borrowing/no-dispose cases that carry no
// leak assertion. Returns the program's exit code (or -999 if it did not run).
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Compile with generated-code AddressSanitizer, run under LeakSanitizer, and
// return the program's exit code — the dispose-exactly-once oracle. A leak /
// double-free / use-after-free FAILS the run (ran=false -> -999), so the
// sanitizer verdict rides on the same assertion as the destructor-count value.
// Consolidates the retired hand-rolled JIT-count + bespoke-LSan pair into the
// one standard checked AOT pipeline.
int runChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}

// Assert the program disposes the removed/surviving elements exactly once (no
// leak, no double-free, no use-after-free) AND returns `expectedExitCode` — both
// verdicts ride on the single checked run.
void expectDisposedAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(runChecked(source), expectedExitCode);
}

// A resource class R with a static disposal counter and a Main holding the body.
std::string prog(const std::string& mainBody) {
    return
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int " + mainBody + " end Main; "
        "end C;";
}

}  // namespace

// ===========================================================================
// M1 — OwningDictionary<string, V>.Remove(key) must dispose the value (§4.9)
// ===========================================================================

// stdlib §4.9: Remove(key: K): bool — disposes the removed value.
// One value is Added then Removed; the destructor must have run once.
// Before the fix: Remove freed only the key, never calling dispose_val →
// destructor counter stayed 0 → test returned 0 instead of 1.
BEZEL_TEST(owned_collection_remove_M1, owned_dict_str_remove_disposes_value) {
    expectDisposedAndLeakFree(prog(
        "using var d := new OwningDictionary<string, R>(); "
        "d.Add(\"x\", move (new R())); "
        "d.Remove(\"x\"); "
        "return R._n;"),        // 1 — destructor ran exactly once on Remove
        1);
}

// stdlib §4.9: two owned values; Remove("a") disposes only that value (count 1
// inside); at scope end the surviving "b" value is disposed (count 2 total).
// Encoded: <count after Remove> * 10 + <count after scope end> = 1*10 + 2 = 12.
BEZEL_TEST(owned_collection_remove_M1,
           owned_dict_str_remove_disposes_exactly_one_and_scope_disposes_survivor) {
    BEZEL_EXPECT_EQ(run(prog(
        "var afterRemove := -1; "
        "do "
        "  using var d := new OwningDictionary<string, R>(); "
        "  d.Add(\"a\", move (new R())); "
        "  d.Add(\"b\", move (new R())); "
        "  d.Remove(\"a\"); "
        "  afterRemove := R._n; "
        "end; "
        "return afterRemove * 10 + R._n;")),
        12);
}

// stdlib §4.9: Remove returns true when the key was present.
BEZEL_TEST(owned_collection_remove_M1,
           owned_dict_str_remove_returns_true_on_hit) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var d := new OwningDictionary<string, R>(); "
        "d.Add(\"x\", move (new R())); "
        "var hit := d.Remove(\"x\"); "
        "return int(hit);")),   // 1
        1);
}

// stdlib §4.9: Remove returns false (and disposes nothing) when the key is absent.
BEZEL_TEST(owned_collection_remove_M1,
           owned_dict_str_remove_absent_key_returns_false_no_dispose) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var d := new OwningDictionary<string, R>(); "
        "d.Add(\"x\", move (new R())); "
        "var hit := d.Remove(\"missing\"); "
        "return int(hit) * 10 + R._n;")),  // 0*10+0 = 0 (not found, not disposed)
        0);
}

// stdlib §4.9 / generic-key path (int key via bz_dict_remove_gen_disposing):
// OwningDictionary<int, V>.Remove(key) also disposes the value.
BEZEL_TEST(owned_collection_remove_M1,
           owned_dict_int_remove_disposes_value) {
    expectDisposedAndLeakFree(prog(
        "using var d := new OwningDictionary<int, R>(); "
        "d.Add(1, move (new R())); "
        "d.Remove(1); "
        "return R._n;"),        // 1 — destructor ran exactly once
        1);
}

// stdlib §4.9 / generic-key path: Remove returns true on hit.
BEZEL_TEST(owned_collection_remove_M1,
           owned_dict_int_remove_returns_true_on_hit) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var d := new OwningDictionary<int, R>(); "
        "d.Add(1, move (new R())); "
        "var hit := d.Remove(1); "
        "return int(hit);")),   // 1
        1);
}

// stdlib §4.8 / non-owning Dictionary: Remove does NOT dispose the element
// (the dictionary borrows; the owner is elsewhere). The borrow source r is a
// `using var` that disposes at Main's scope end — exactly once total.
// Encoded: int(hit) * 100 + d.Count * 10 + R._n = 1*100 + 0*10 + 0 = 100.
// (R._n == 0 at the return: the destructor has not run yet because `using var r`
// is still in scope until after the return.)
BEZEL_TEST(owned_collection_remove_M1,
           nonowning_dict_remove_does_not_dispose) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var d := new Dictionary<string, R>(); "
        "using var r := new R(); "
        "d.Add(\"x\", r); "
        "var hit := d.Remove(\"x\"); "
        "return int(hit) * 100 + d.Count * 10 + R._n;")),
        // 1*100 + 0*10 + 0 = 100  (destructor NOT called by Remove)
        100);
}

// ===========================================================================
// M2 — OwningLinkedList<T>.Remove(item) must find, dispose, and unlink (§4.6/§4.3)
// ===========================================================================

// stdlib §4.6 / §4.3: Remove(item): bool — disposes the matched element and
// returns true. Before the fix Remove was a no-op (no m == "Remove" case →
// null result); destructor never ran → returned 0 instead of 1.
// The needle is borrowed from the list itself (`lst.First`, a borrow per stdlib
// §4.7): the added element is owned by the list, so a valid identity-equal
// needle can only come from the list. (Binding a pre-`move` borrow of the added
// value and using it after the `move` is a consume-site-freeze error, §28.3
// v0.180 — it would read freed memory once Remove disposes the element; the JIT
// harness ran that shape only because it skips the ownership pass.)
BEZEL_TEST(owned_collection_remove_M2,
           owned_linkedlist_remove_disposes_element) {
    expectDisposedAndLeakFree(prog(
        "using var lst := new OwningLinkedList<R>(); "
        "lst.AddLast(move (new R())); "
        "lst.Remove(lst.First!); "
        "return R._n;"),        // 1 — destructor ran exactly once on Remove
        1);
}

// stdlib §4.6/§4.3: after Remove the Count decreases.
// One element added, one removed → Count == 0.
BEZEL_TEST(owned_collection_remove_M2,
           owned_linkedlist_remove_decreases_count) {
    BEZEL_EXPECT_EQ(run(
        "module M; class R end R; "
        "class C public static method Main(): int "
        "  using var lst := new OwningLinkedList<R>(); "
        "  lst.AddLast(move (new R())); "
        "  lst.Remove(lst.First!); "
        "  return lst.Count; "  // 0 after Remove
        "end Main; end C;"),
        0);
}

// stdlib §4.6/§4.3: Remove(item) returns true when the item is found.
BEZEL_TEST(owned_collection_remove_M2,
           owned_linkedlist_remove_returns_true_on_hit) {
    BEZEL_EXPECT_EQ(run(
        "module M; class R end R; "
        "class C public static method Main(): int "
        "  using var lst := new OwningLinkedList<R>(); "
        "  lst.AddLast(move (new R())); "
        "  var hit := lst.Remove(lst.First!); "
        "  return int(hit);"    // 1
        "end Main; end C;"),
        1);
}

// stdlib §4.6/§4.3: Remove(item) on an absent item returns false and disposes
// nothing. Two elements added; a fresh (never-stored) borrow is passed as the
// needle — it is pointer-distinct from every stored node.
BEZEL_TEST(owned_collection_remove_M2,
           owned_linkedlist_remove_absent_item_returns_false_no_dispose) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var lst := new OwningLinkedList<R>(); "
        "lst.AddLast(move (new R())); "
        "lst.AddLast(move (new R())); "
        "using var probe := new R(); "     // never added to lst
        "var hit := lst.Remove(probe); "   // pointer-distinct → not found
        "return int(hit) * 100 + lst.Count * 10 + R._n;")),
        // 0*100 + 2*10 + 0 = 20  (not found, two survivors, destructor untouched)
        20);
}

// stdlib §4.6/§4.3: remove the first of two elements; the second survives.
// Dispose count after Remove == 1; after scope end == 2. Encoded 12.
BEZEL_TEST(owned_collection_remove_M2,
           owned_linkedlist_remove_first_of_two_scope_disposes_survivor) {
    BEZEL_EXPECT_EQ(run(prog(
        "var afterRemove := -1; "
        "do "
        "  using var lst := new OwningLinkedList<R>(); "
        "  lst.AddLast(move (new R())); "
        "  lst.AddLast(move (new R())); "
        "  lst.Remove(lst.First!); "  // disposes the first element → R._n == 1
        "  afterRemove := R._n; "
        "end; "
        "return afterRemove * 10 + R._n;")),
        12);
}

// stdlib §4.6: borrowing LinkedList<T>.Remove(item) unlinks the node but does
// NOT dispose the element (borrows are owned elsewhere). Three class elements
// stored; Remove(b2) removes without disposing. Count == 2; R._n still 0.
// Encoded: int(hit)*100 + Count*10 + R._n = 1*100 + 2*10 + 0 = 120.
BEZEL_TEST(owned_collection_remove_M2,
           nonowning_linkedlist_remove_does_not_dispose) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var lst := new LinkedList<R>(); "
        "using var r1 := new R(); "
        "using var r2 := new R(); "
        "using var r3 := new R(); "
        "lst.AddLast(r1); lst.AddLast(r2); lst.AddLast(r3); "
        "var hit := lst.Remove(r2); "
        "return int(hit) * 100 + lst.Count * 10 + R._n;")),
        // 1*100 + 2*10 + 0 = 120  (hit, count 2, no destructor called by Remove)
        120);
}

// ===========================================================================
// M3 — `var removed := lst.Remove(b); if removed then ...` must compile (§4.6)
// ===========================================================================

// stdlib §4.6/§4.3: Using the Remove result in a conditional must produce a
// proper i1 bool — not i32 — so the branch verifier does not crash. Before the
// M2 fix, Remove returned nullptr from codegen → the branch value was a wrong
// type → IR verifier error ("Branch condition is not 'i1' type! br i32 %15").
BEZEL_TEST(owned_collection_remove_M3,
           owned_linkedlist_remove_result_used_as_condition_compiles_correctly) {
    BEZEL_EXPECT_EQ(run(
        "module M; class R end R; "
        "class C public static method Main(): int "
        "  using var lst := new OwningLinkedList<R>(); "
        "  lst.AddLast(move (new R())); "
        "  var removed := lst.Remove(lst.First!); "
        "  return (if removed then 1 else 0);"   // must compile + run correctly
        "end Main; end C;"),
        1);   // Remove found and removed the element → removed == true → 1
}

// stdlib §4.6/§4.3: same pattern, Remove on an absent item → false → 0.
BEZEL_TEST(owned_collection_remove_M3,
           owned_linkedlist_remove_absent_result_as_condition) {
    BEZEL_EXPECT_EQ(run(
        "module M; class R end R; "
        "class C public static method Main(): int "
        "  using var lst := new OwningLinkedList<R>(); "
        "  lst.AddLast(move (new R())); "
        "  using var probe := new R(); "  // not stored in lst
        "  var removed := lst.Remove(probe); "
        "  return (if removed then 1 else 0);"
        "end Main; end C;"),
        0);   // probe not found → removed == false → 0
}
