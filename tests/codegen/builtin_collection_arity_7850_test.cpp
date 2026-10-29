// Regression tests for issue #7850 — the same hole as #7810 on the other
// built-in collections (`Set`, `Queue`, `Stack`, `LinkedList`, `SortedSet`,
// `Dictionary`, `SortedDictionary`, `OwningArray`, `OwningDictionary`). One
// arity gate (see builtin_collection_arity_7810_test.cpp) covers both issues;
// these rows pin the members #7850 names and the shapes beyond `Array<T>`.
//
// stdlib §4.5-§4.11, §4.17, §4.18 (v0.350) declare, e.g.:
//
//     Set<T>.Contains(item: T): bool;      Set<T>.TryAdd(item: T): bool;
//     Queue<T>.Peek(): T;                  Queue<T>.TryPeek(out item: T?): bool;
//     Dictionary<K, V>.TryAdd(key: K, value: V): bool;
//     OwningArray<T>.IndexOf(item: T): int;     // no startIndex, unlike Array<T>
//
// Besides the #3079 warning-and-nothing-emitted shape, a zero-parameter member
// (`q.Peek(1)`, `st.Pop(1)`) compiled and DROPPED the surplus argument.
//
// The REJECT rows assert the specific diagnostic; the ACCEPT row reads the
// data of every declared arity.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

std::string prog(const std::string& decls, const std::string& body) {
    return "module M;\nimport System;\nimport System.Collections.Generic;\n"
           "import System.IO;\n" + decls +
           "class P\n"
           "  public static method Main(): int\n" + body + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end P;\n";
}

// The program is rejected, with an Error that names `callee`, the supplied
// count and the declared count.
void expectArityRejected(const std::string& decls, const std::string& body,
                         const std::string& callee, int supplied,
                         const std::string& declared) {
    auto r = runAot(prog(decls, body));
    BEZEL_EXPECT_FALSE(r.ran);
    const std::string want = "the call to `" + callee + "` supplies " +
                             std::to_string(supplied) + " argument(s)";
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(want) != std::string::npos &&
            d.message.find("with " + declared + " argument(s)") !=
                std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}

void expectStdout(const std::string& decls, const std::string& body,
                  const std::string& out) {
    auto r = runAot(prog(decls, body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

}  // namespace

BEZEL_TEST(builtin_collection_arity_7850, set_contains_two_args_is_rejected) {
    // per stdlib §4.10 (v0.350) `Contains(item: T)` via ICollection<T>
    expectArityRejected("", "    var s := new Set<int>();\n"
                            "    Console.WriteLine(s.Contains(1, 2));",
                        "Set.Contains", 2, "exactly 1");
}

BEZEL_TEST(builtin_collection_arity_7850, set_tryadd_no_args_is_rejected) {
    // per stdlib §4.10 (v0.350) `TryAdd(item: T): bool`
    expectArityRejected("", "    var s := new Set<int>();\n"
                            "    Console.WriteLine(s.TryAdd());",
                        "Set.TryAdd", 0, "exactly 1");
}

// The silent-DROP shape: surplus arguments to a zero-parameter member
// compiled and the argument vanished.
BEZEL_TEST(builtin_collection_arity_7850, queue_peek_with_an_argument_is_rejected) {
    // per stdlib §4.11 (v0.350) `Queue<T>.Peek(): T`
    expectArityRejected("", "    var q := new Queue<int>();\n"
                            "    q.Enqueue(4);\n"
                            "    Console.WriteLine(q.Peek(1));",
                        "Queue.Peek", 1, "exactly 0");
}

BEZEL_TEST(builtin_collection_arity_7850, stack_peek_with_an_argument_is_rejected) {
    // per stdlib §4.11 (v0.350) `Stack<T>.Peek(): T`
    expectArityRejected("", "    var s := new Stack<int>();\n"
                            "    s.Push(4);\n"
                            "    Console.WriteLine(s.Peek(1));",
                        "Stack.Peek", 1, "exactly 0");
}

BEZEL_TEST(builtin_collection_arity_7850, stack_pop_with_an_argument_is_rejected) {
    // per stdlib §4.11 (v0.350) `Stack<T>.Pop(): T`
    expectArityRejected("", "    var s := new Stack<int>();\n"
                            "    Console.WriteLine(s.Pop(1));",
                        "Stack.Pop", 1, "exactly 0");
}

BEZEL_TEST(builtin_collection_arity_7850, queue_trypeek_without_out_slot_is_rejected) {
    // per stdlib §4.11 (v0.350) `TryPeek(out item: T?): bool`
    expectArityRejected("", "    var q := new Queue<int>();\n"
                            "    Console.WriteLine(q.TryPeek());",
                        "Queue.TryPeek", 0, "exactly 1");
}

BEZEL_TEST(builtin_collection_arity_7850, linkedlist_find_two_args_is_rejected) {
    // per stdlib §4.6 (v0.350) `Find(item: T): LinkedListNode<T>?`
    expectArityRejected("", "    var l := new LinkedList<int>();\n"
                            "    var n := l.Find(1, 2);",
                        "LinkedList.Find", 2, "exactly 1");
}

BEZEL_TEST(builtin_collection_arity_7850, sortedset_remove_two_args_is_rejected) {
    // per stdlib §4.18 (v0.350) `Remove(item: T): bool`
    expectArityRejected("", "    var s := new SortedSet<int>();\n"
                            "    Console.WriteLine(s.Remove(1, 2));",
                        "SortedSet.Remove", 2, "exactly 1");
}

BEZEL_TEST(builtin_collection_arity_7850, dictionary_containskey_two_args_is_rejected) {
    // per stdlib §4.8 (v0.350) `ContainsKey(key: K): bool`
    expectArityRejected("", "    var d := new Dictionary<int, int>();\n"
                            "    Console.WriteLine(d.ContainsKey(1, 2));",
                        "Dictionary.ContainsKey", 2, "exactly 1");
}

BEZEL_TEST(builtin_collection_arity_7850, dictionary_tryadd_one_arg_is_rejected) {
    // per stdlib §4.8 (v0.350) `TryAdd(key: K, value: V): bool`
    expectArityRejected("", "    var d := new Dictionary<int, int>();\n"
                            "    Console.WriteLine(d.TryAdd(1));",
                        "Dictionary.TryAdd", 1, "exactly 2");
}

BEZEL_TEST(builtin_collection_arity_7850, sorteddictionary_getornull_two_args_is_rejected) {
    // per stdlib §4.17 (v0.350) `GetOrNull(key: K): V?`
    expectArityRejected("", "    var d := new SortedDictionary<int, int>();\n"
                            "    var v := d.GetOrNull(1, 2);",
                        "SortedDictionary.GetOrNull", 2, "exactly 1");
}

BEZEL_TEST(builtin_collection_arity_7850, owningarray_indexof_two_args_is_rejected) {
    // per stdlib §4.5 (v0.350): `OwningArray<T>.IndexOf(item: T)` has NO
    // `startIndex` (only `Array<T>` does)
    expectArityRejected("class Item end Item;\n",
                        "    using var o := new OwningArray<Item>();\n"
                        "    var it := new Item();\n"
                        "    Console.WriteLine(o.IndexOf(it, 1));",
                        "OwningArray.IndexOf", 2, "exactly 1");
}

BEZEL_TEST(builtin_collection_arity_7850, owningdictionary_contains_two_args_is_rejected) {
    // per stdlib §4.9 (v0.350) `Contains(key: K): bool`
    expectArityRejected("class Item end Item;\n",
                        "    using var d := new OwningDictionary<int, Item>();\n"
                        "    Console.WriteLine(d.Contains(1, 2));",
                        "OwningDictionary.Contains", 2, "exactly 1");
}

// Every DECLARED arity of the same members still runs and reads its data.
BEZEL_TEST(builtin_collection_arity_7850, declared_arities_run) {
    // per stdlib §4.6, §4.8, §4.10, §4.11 (v0.350)
    expectStdout("",
                 "    var s := new Set<int>();\n"
                 "    s.Add(1);\n"
                 "    Console.WriteLine(s.TryAdd(1));\n"
                 "    Console.WriteLine(s.Contains(1));\n"
                 "    Console.WriteLine(s.Remove(1));\n"
                 "    var q := new Queue<int>();\n"
                 "    q.Enqueue(4);\n"
                 "    Console.WriteLine(q.Peek());\n"
                 "    var x: int? := null;\n"
                 "    Console.WriteLine(q.TryPeek(out x));\n"
                 "    Console.WriteLine(q.Dequeue());\n"
                 "    var st := new Stack<int>();\n"
                 "    st.Push(7);\n"
                 "    Console.WriteLine(st.Peek());\n"
                 "    Console.WriteLine(st.Pop());\n"
                 "    var d := new Dictionary<int, int>();\n"
                 "    d.Add(1, 2);\n"
                 "    Console.WriteLine(d.TryAdd(1, 3));\n"
                 "    Console.WriteLine(d.ContainsKey(1));\n"
                 "    Console.WriteLine(d.GetOrNull(1) ?? -1);\n"
                 "    var v: int? := null;\n"
                 "    Console.WriteLine(d.TryGetValue(1, out v));\n"
                 "    Console.WriteLine(d.Remove(1));\n"
                 "    var ll := new LinkedList<int>();\n"
                 "    ll.AddLast(5);\n"
                 "    Console.WriteLine(ll.Contains(5));",
                 "False\nTrue\nTrue\n4\nTrue\n4\n7\n7\nFalse\nTrue\n2\nTrue\nTrue\nTrue\n");
}

// CONTROL: a USER type's `Contains` takes the arity it declares; the gate
// reads built-in heads only.
BEZEL_TEST(builtin_collection_arity_7850, control_user_type_member_keeps_its_own_arity) {
    // per spec §22.5 (a user method is bound by its own declaration)
    expectStdout("class Bag\n"
                 "  public method Contains(a: int, b: int): bool => a = b;\n"
                 "end Bag;\n",
                 "    var b := new Bag();\n"
                 "    Console.WriteLine(b.Contains(2, 2));",
                 "True\n");
}

