// Codegen / disposal — a borrowing `LinkedList<string?>` never took or released
// the +1 per stored string element that `LinkedList<string>` does (every
// linked-list string-element predicate carried a `!nullable` gate), so:
//
//   * a BORROWED element stored by `AddLast` / `AddFirst` was stored without a
//     reference: it died with its source and every later read of the slot was
//     a heap-use-after-free;
//   * a +1 string PRODUCER stored by the same call was orphaned — neither
//     retained by the slot nor transferred into it — and leaked;
//   * the SOURCE CONSTRUCTOR (`new LinkedList<string?>(source)`) did the same.
//
// Issue #9107.
//
// Spec: §3.6 "Derived type properties" *Nullable forms* (the ELEMENT CATEGORY
// of a nullable type `V?` is `V`'s, nullability stripped), §28.9 "Runtime-
// managed types" (a `string` is refcounted; null is a retain/release no-op),
// §28.11 "Collection element ownership" *The element slot policy* (the
// runtime-managed-reference row's borrowing-family column is "shared
// references": the collection owns a +1 of its own on the element and releases
// it when the slot goes away — and the slot policy is a PAIR: the store takes
// the reference, the collection's free gives it back). Spec revision v0.359.
//
// Model: #9064 (the same gate on the array-backed family), and the
// `Dictionary<K, string?>` value twin (#8207).
//
// Every string is built at run time (a folded literal is an RC sentinel on
// which release is a no-op and would hide the leak). runAotChecked runs under
// ASan/LSan, so both a leak (the bug) and a double-free / use-after-free flip
// `r.ran`; stdout pins the values (a use-after-free on read-back would
// corrupt them).
//
// ROW BASES (each row is RED without this fix):
//   * array_elements_added_to_a_linkedlist_are_co_owned_... — RED on the base
//     r30-c1 (the array's element was released at the array's free and the
//     linked list read it afterwards: a heap-use-after-free).
//   * source_constructor_co_owns_the_elements_... — RED on the base r30-c1
//     (the same shape through `new LinkedList<string?>(source)`).
//   * dictionary_value_added_to_a_linkedlist_is_co_owned_... — RED on the base
//     r30-c1 (the dictionary's value died with the dictionary).
//   * producer_element_is_not_orphaned_... — RED on the base r30-c1 (the +1
//     producer's reference was orphaned: a leak).
//   * the two control rows are green on the base and on this fix; each is RED
//     under the wrong fix the commit body names.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// The issue's own repro: a BORROWED `string` stored by `LinkedList<string?>`.
// The list must take a +1 of its own, or the element dies with the caller's
// scope and the read-back is a use-after-free.
const char* kBorrowedElement =
    "module M; import System;\n"
    "public class C\n"
    "  public static method Fill(l: LinkedList<string?>, s: string): int\n"
    "    l.Add(s);\n"
    "    return 0;\n"
    "  end Fill;\n"
    "  public static method Main(): int\n"
    "    var l := new LinkedList<string?>();\n"
    "    var tot := 0;\n"
    "    for var i in 0..<200 do\n"
    "      var s := \"w\" + i.ToString();\n"
    "      C.Fill(l, s);\n"
    "    end;\n"
    "    for var e in l do tot := tot + (e ?? \"\").Length; end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// The elements of a DYING `Array<string?>` moved into a linked list: the
// array's free gives its reference back (§28.11's pair), so the list must hold
// one of its own — otherwise the read-back is a use-after-free. "x" + i is 2
// characters for i < 10, 3 for i < 100 and 4 above: 20 + 270 + 400 = 690.
const char* kArrayElements =
    "module M; import System;\n"
    "public class C\n"
    "  public static method Fill(l: LinkedList<string?>, i: int)\n"
    "    var a := new Array<string?>();\n"
    "    a.Add(\"x\" + i.ToString()); a.Add(null);\n"
    "    for var e in a do l.AddLast(e); end;\n"
    "  end Fill;\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    for var i in 0..<200 do\n"
    "      var l := new LinkedList<string?>();\n"
    "      C.Fill(l, i);\n"
    "      for var e in l do tot := tot + (e ?? \"\").Length; end;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// The SOURCE CONSTRUCTOR (`new LinkedList<string?>(source)`) is the third
// insert path: it appends the drained source, so it must take the list's +1
// per element too.
const char* kSourceConstructor =
    "module M; import System;\n"
    "public class C\n"
    "  public static method Make(i: int): owned LinkedList<string?>\n"
    "    var a := new Array<string?>();\n"
    "    a.Add(\"x\" + i.ToString()); a.Add(null);\n"
    "    var l := new LinkedList<string?>(a);\n"
    "    return l;\n"
    "  end Make;\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    for var i in 0..<200 do\n"
    "      var l := C.Make(i);\n"
    "      for var e in l do tot := tot + (e ?? \"\").Length; end;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// A DYING `Dictionary<int, string?>`'s VALUE moved into a linked list: the
// dictionary's value is co-owned by the dictionary, so the list must take a
// +1 of its own.
const char* kDictionaryValue =
    "module M; import System;\n"
    "public class C\n"
    "  public static method Fill(l: LinkedList<string?>, i: int)\n"
    "    var d := new Dictionary<int, string?>();\n"
    "    d.Add(1, \"x\" + i.ToString());\n"
    "    l.AddLast(d[1]);\n"
    "  end Fill;\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    for var i in 0..<200 do\n"
    "      var l := new LinkedList<string?>();\n"
    "      C.Fill(l, i);\n"
    "      for var e in l do tot := tot + (e ?? \"\").Length; end;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// The LEAK direction (the issue's second repro): a +1 producer stored in a
// `Task<LinkedList<string?>>`'s result. The producer's reference is
// TRANSFERRED into the list, so the list's free must give it back — otherwise
// it is orphaned (the async Pattern-A task-result disposer used to keep its
// `!nullable` gate, which is why this row leaked on the base).
const char* kAsyncProducer =
    "module M; import System;\n"
    "public class C\n"
    "  public static async method Make(i: int): Task<LinkedList<string?>>\n"
    "    var r := new LinkedList<string?>();\n"
    "    r.Add(\"w\" + i.ToString());\n"
    "    return r;\n"
    "  end Make;\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    for var i in 0..<200 do\n"
    "      using var t := C.Make(i);\n"
    "      t.Wait();\n"
    "      tot := tot + t.Result.Count;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// Control: the async Pattern-A task result holds a BORROWED element. The list
// now retains it, so the task result's free gives that +1 back — but only
// ONE of them. This row is RED under the wrong fix that enables the release
// without the matching retain (the rejected commit feb164d20 for #9064:
// "bezel: fatal: refcount over-release (string)", rc 134).
const char* kControlTaskBorrowed =
    "module M; import System;\n"
    "public class C\n"
    "  public static async method Make(s: string): Task<LinkedList<string?>>\n"
    "    var r := new LinkedList<string?>();\n"
    "    r.Add(s);\n"
    "    return r;\n"
    "  end Make;\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    for var i in 0..<200 do\n"
    "      var s := \"w\" + i.ToString();\n"
    "      using var t := C.Make(s);\n"
    "      t.Wait();\n"
    "      tot := tot + t.Result.Count + s.Length;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// Control: the NON-NULLABLE twin is unchanged — it already took and released
// the +1 per element, and the nullable form now behaves exactly like it.
// This row is RED under a wrong fix that narrows the retain to the nullable
// form only (the twin would then leak one reference per element).
//
// The element is passed through a METHOD PARAMETER (a borrow), like the
// nullable row above: a local's own +1 is a separate, pre-existing defect
// (the local's scope-end release, #8661's class) that this fix does not touch
// and that this row must not pin.
const char* kControlNonNullable =
    "module M; import System;\n"
    "public class C\n"
    "  public static method Fill(l: LinkedList<string>, s: string): int\n"
    "    l.Add(s);\n"
    "    return 0;\n"
    "  end Fill;\n"
    "  public static method Main(): int\n"
    "    var l := new LinkedList<string>();\n"
    "    var tot := 0;\n"
    "    for var i in 0..<200 do\n"
    "      var s := \"w\" + i.ToString();\n"
    "      C.Fill(l, s);\n"
    "    end;\n"
    "    for var e in l do tot := tot + e.Length; end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

void expectClean(const char* src, const char* out) {
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);          // no leak (the bug) and no double-free
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(out));
}

}  // namespace

BEZEL_TEST(ownership_linkedlist_nullable_string_element_coownership_9107,
           borrowed_element_takes_its_own_reference_spec_28_11) {
    expectClean(kBorrowedElement, "690\n");
}

BEZEL_TEST(ownership_linkedlist_nullable_string_element_coownership_9107,
           array_elements_added_to_a_linkedlist_are_co_owned_spec_28_11) {
    expectClean(kArrayElements, "690\n");
}

BEZEL_TEST(ownership_linkedlist_nullable_string_element_coownership_9107,
           source_constructor_co_owns_the_elements_spec_28_11) {
    expectClean(kSourceConstructor, "690\n");
}

BEZEL_TEST(ownership_linkedlist_nullable_string_element_coownership_9107,
           dictionary_value_added_to_a_linkedlist_is_co_owned_spec_28_11) {
    expectClean(kDictionaryValue, "690\n");
}

BEZEL_TEST(ownership_linkedlist_nullable_string_element_coownership_9107,
           producer_element_is_not_orphaned_spec_28_9) {
    expectClean(kAsyncProducer, "200\n");
}

BEZEL_TEST(ownership_linkedlist_nullable_string_element_coownership_9107,
           control_task_result_borrowed_element_is_not_over_released_spec_28_9) {
    expectClean(kControlTaskBorrowed, "890\n");
}

BEZEL_TEST(ownership_linkedlist_nullable_string_element_coownership_9107,
           control_nonnullable_linkedlist_twin_unchanged_spec_28_9) {
    expectClean(kControlNonNullable, "690\n");
}
