// Codegen -- stdlib section 5 (v0.355) `ElementAtOrNull(index: int): T?` on
// the CONCRETE collection receivers, issue 8232.
//
// THE DEFECT. stdlib section 5 declares, inside
// `public extension IEnumerable<T>`:
//
//   public method ElementAt(index: int): T;
//   public method ElementAtOrNull(index: int): T?;      // -- null when index
//                                                        //    is out of range
//
// Every concrete collection is an `IEnumerable<T>` (`Array<T>`/`OwningArray<T>`
// section 4.4/4.5, `LinkedList<T>`/`OwningLinkedList<T>` section 4.6/4.7,
// `Set<T>`/`OwningSet<T>` section 4.10, `SortedSet<T>` section 4.18,
// `Dictionary<K,V>`/`SortedDictionary<K,V>` section 4.8/4.17), so the call
// should be admitted on all of them -- and on a `string`, which is an
// `IEnumerable<char>` (section 9.6 / #1220).
//
// MEASURED at the base commit (r30-c1, 866a6d39), `build/bezelc`:
//
//   Array<int>.ElementAtOrNull(1)
//     -> "no member `ElementAtOrNull` on `Array<T>` (section 22.5 / section
//         4.x) -- it is not part of this collection's method surface, a
//         universal Object member, or a LINQ operator"
//   LinkedList<int> / Set<int> / SortedSet<int> / Dictionary<int,int>
//     -> the same wording
//   "abc".ElementAtOrNull(1)
//     -> "string member `.ElementAtOrNull` is not yet supported in codegen"
//
// ROOT CAUSE. sema's concrete-collection admission gate
// (`ExprTypecheck_Calls.cpp`'s `allowedOwningMethod`/`allowedNonOwningMethod`)
// ends with `common::isBuiltinCollectionLinqOperatorName(mm)`, and
// `kBuiltinCollectionLinqOperatorNames` (`src/common/BuiltinRegistry.cpp`)
// did NOT list `ElementAtOrNull` -- so the name never reached a typing rule
// at all. The string receiver's gate reads the sibling table
// `kStringLinqOperatorMemberNames`, which did not list it either.
//
// The CODEGEN half was NOT the gap: `LlvmBackend_Calls.cpp`'s `isArr` block
// already carries the `T?` `ElementAtOrNull` arm (fix #8229), and the
// LinkedList / Set / SortedSet / string receivers reach that SAME arm through
// the `lazyTerminal` materialize path (the snapshot -> array -> terminal
// route), exactly as `ElementAt` itself does.
//
// The rows below pin the ADMISSION and the VALUES on every concrete receiver
// family, the out-of-range and negative-index shapes (null, NOT a throw --
// `ElementAt(i)` throws `IndexOutOfRangeException` there), the string
// receiver, and the leak-freedom of the materialized snapshot over 100
// iterations.

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

void expectOut(const JitResult& r, const char* out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

void expectNoLeak(const JitResult& r, const char* out) {
    expectOut(r, out);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary = bezel::test::aot_detail::parseLeakSummary(
        diagnosticText(r), leakedBytes, leakedAllocs);
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocs << " allocation(s) / "
                  << leakedBytes << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_EQ(sawSummary ? leakedAllocs : 0ull, 0ull);
    BEZEL_EXPECT_EQ(sawSummary ? leakedBytes : 0ull, 0ull);
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
}

// A program with one `Array<int>` receiver holding 10, 20, 30.
std::string prog(const std::string& body) {
    return "module M; import System; import System.Collections.Generic;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    using var xs := new Array<int>();\n"
           "    xs.Add(10); xs.Add(20); xs.Add(30);\n" +
           body + "\n    return 0;\n  end Main;\nend C;\n";
}

}  // namespace

// THE ISSUE'S REPRO, verbatim (issue 8232's own program). Expected 20 then
// -1: the element at index 1, then null (out of range) at index 2.
BEZEL_TEST(collection_elementatornull_8232, issue_repro_verbatim_stdlib_5) {
    expectOut(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class C
  public static method Main(): int
    using var xs := new Array<int>();
    xs.Add(10); xs.Add(20);
    Console.WriteLine(xs.ElementAtOrNull(1) ?? -1);
    Console.WriteLine(xs.ElementAtOrNull(2) ?? -1);
    return 0;
  end Main;
end C;
)BZL"),
              "20\n-1\n");
}

// stdlib section 5: `ElementAtOrNull(i)` is null when the index is out of
// range -- negative, equal to the count, or past it -- where `ElementAt(i)`
// throws `IndexOutOfRangeException`.
BEZEL_TEST(collection_elementatornull_8232, array_out_of_range_and_negative_is_null) {
    expectOut(runAotChecked(prog(
        "    Console.WriteLine(xs.ElementAtOrNull(0) ?? -1);\n"
        "    Console.WriteLine(xs.ElementAtOrNull(2) ?? -1);\n"
        "    Console.WriteLine(xs.ElementAtOrNull(3) ?? -1);\n"
        "    Console.WriteLine(xs.ElementAtOrNull(-1) ?? -1);\n"
        "    Console.WriteLine(xs.ElementAtOrNull(-100) ?? -1);")),
        "10\n30\n-1\n-1\n-1\n");
}

// A `string?` element: the result is a nullable reference, so `??` composes
// with a string fallback.
BEZEL_TEST(collection_elementatornull_8232, array_string_element_is_nullable_string) {
    expectOut(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class C
  public static method Main(): int
    using var xs := new Array<string>();
    xs.Add("a"); xs.Add("bb"); xs.Add("ccc");
    Console.WriteLine(xs.ElementAtOrNull(1) ?? "none");
    Console.WriteLine(xs.ElementAtOrNull(3) ?? "none");
    Console.WriteLine(xs.ElementAtOrNull(-1) ?? "none");
    return 0;
  end Main;
end C;
)BZL"),
              "bb\nnone\nnone\n");
}

// `OwningArray<T>` (stdlib section 4.5) -- the owning twin of `Array<T>`.
// An owning collection requires a CLASS element type (stdlib section 4.2 /
// section 14.4), so the element is a heap object.
BEZEL_TEST(collection_elementatornull_8232, owning_array_receiver) {
    expectOut(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class Item
  public V: int;
  public constructor(v: int)
    V := v;
  end constructor;
  destructor end destructor;
end Item;
class C
  public static method Main(): int
    using var xs := new OwningArray<Item>();
    xs.Add(move (new Item(10)));
    xs.Add(move (new Item(20)));
    xs.Add(move (new Item(30)));
    var v := xs.ElementAtOrNull(1);
    var w := xs.ElementAtOrNull(3);
    Console.WriteLine(v!.V);
    Console.WriteLine(w = null);
    return 0;
  end Main;
end C;
)BZL"),
              "20\nTrue\n");
}

// `LinkedList<T>` (stdlib section 4.6) -- a `bz_llist_t`-backed receiver, so
// the terminal materializes the list into a snapshot array first.
BEZEL_TEST(collection_elementatornull_8232, linked_list_receiver) {
    expectOut(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class C
  public static method Main(): int
    using var xs := new LinkedList<int>();
    xs.AddLast(10); xs.AddLast(20); xs.AddLast(30);
    Console.WriteLine(xs.ElementAtOrNull(1) ?? -1);
    Console.WriteLine(xs.ElementAtOrNull(2) ?? -1);
    Console.WriteLine(xs.ElementAtOrNull(3) ?? -1);
    Console.WriteLine(xs.ElementAtOrNull(-1) ?? -1);
    return 0;
  end Main;
end C;
)BZL"),
              "20\n30\n-1\n-1\n");
}

// `OwningLinkedList<T>` (stdlib section 4.7).
BEZEL_TEST(collection_elementatornull_8232, owning_linked_list_receiver) {
    expectOut(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class Item
  public V: int;
  public constructor(v: int)
    V := v;
  end constructor;
  destructor end destructor;
end Item;
class C
  public static method Main(): int
    using var xs := new OwningLinkedList<Item>();
    xs.AddLast(move (new Item(10)));
    xs.AddLast(move (new Item(20)));
    var v := xs.ElementAtOrNull(1);
    var w := xs.ElementAtOrNull(2);
    Console.WriteLine(v!.V);
    Console.WriteLine(w = null);
    return 0;
  end Main;
end C;
)BZL"),
              "20\nTrue\n");
}

// `Set<T>` (stdlib section 4.10) -- an unordered hash set; the terminal
// materializes it through the same snapshot path.
BEZEL_TEST(collection_elementatornull_8232, set_receiver) {
    expectOut(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class C
  public static method Main(): int
    using var xs := new Set<int>();
    xs.Add(10); xs.Add(20); xs.Add(30);
    Console.WriteLine(xs.ElementAtOrNull(0) ?? -1);
    Console.WriteLine(xs.ElementAtOrNull(3) ?? -1);
    Console.WriteLine(xs.ElementAtOrNull(-1) ?? -1);
    return 0;
  end Main;
end C;
)BZL"),
              "10\n-1\n-1\n");
}

// `OwningSet<T>` (stdlib section 4.10).
BEZEL_TEST(collection_elementatornull_8232, owning_set_receiver) {
    expectOut(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class Item
  public V: int;
  public constructor(v: int)
    V := v;
  end constructor;
  destructor end destructor;
end Item;
class C
  public static method Main(): int
    using var xs := new OwningSet<Item>();
    xs.Add(new Item(10));
    xs.Add(new Item(20));
    var v := xs.ElementAtOrNull(0);
    var w := xs.ElementAtOrNull(2);
    Console.WriteLine(v!.V);
    Console.WriteLine(w = null);
    return 0;
  end Main;
end C;
)BZL"),
              "10\nTrue\n");
}

// `SortedSet<T>` (stdlib section 4.18) -- an ordered set; its iteration is in
// ascending order, so the element at index 0 is the smallest.
BEZEL_TEST(collection_elementatornull_8232, sorted_set_receiver) {
    expectOut(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class C
  public static method Main(): int
    using var xs := new SortedSet<int>();
    xs.Add(30); xs.Add(10); xs.Add(20);
    Console.WriteLine(xs.ElementAtOrNull(0) ?? -1);
    Console.WriteLine(xs.ElementAtOrNull(2) ?? -1);
    Console.WriteLine(xs.ElementAtOrNull(3) ?? -1);
    return 0;
  end Main;
end C;
)BZL"),
              "10\n30\n-1\n");
}

// `Stack<T>` (stdlib section 4.11) -- an `IReadOnlyCollection<T>`, so
// section 5's extension applies to it too.
BEZEL_TEST(collection_elementatornull_8232, stack_receiver) {
    expectOut(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class C
  public static method Main(): int
    using var xs := new Stack<int>();
    xs.Push(10); xs.Push(20); xs.Push(30);
    Console.WriteLine(xs.ElementAtOrNull(0) ?? -1);
    Console.WriteLine(xs.ElementAtOrNull(3) ?? -1);
    return 0;
  end Main;
end C;
)BZL"),
              "10\n-1\n");
}

// `Queue<T>` (stdlib section 4.11).
BEZEL_TEST(collection_elementatornull_8232, queue_receiver) {
    expectOut(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class C
  public static method Main(): int
    using var xs := new Queue<int>();
    xs.Enqueue(10); xs.Enqueue(20); xs.Enqueue(30);
    Console.WriteLine(xs.ElementAtOrNull(0) ?? -1);
    Console.WriteLine(xs.ElementAtOrNull(3) ?? -1);
    return 0;
  end Main;
end C;
)BZL"),
              "10\n-1\n");
}

// A `string` receiver (stdlib section 9.6: a string is an
// `IEnumerable<char>`), so section 5's `ElementAtOrNull` applies and the
// result is a `char?`.
BEZEL_TEST(collection_elementatornull_8232, string_receiver_is_nullable_char) {
    expectOut(runAotChecked(R"BZL(
module M; import System;
class C
  public static method Main(): int
    var c := "abc".ElementAtOrNull(1);
    var d := "abc".ElementAtOrNull(3);
    Console.WriteLine(int(c!));
    Console.WriteLine(d = null);
    return 0;
  end Main;
end C;
)BZL"),
              "98\nTrue\n");
}

// A record element: the `T?` is the tagged {hasValue, value} struct, and the
// unwrapped value is the record.
BEZEL_TEST(collection_elementatornull_8232, record_element) {
    expectOut(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
record Pt(X: int, Y: int);
class C
  public static method Main(): int
    using var xs := new Array<Pt>();
    xs.Add(new Pt(1, 2)); xs.Add(new Pt(3, 4)); xs.Add(new Pt(5, 6));
    var v := xs.ElementAtOrNull(1);
    var w := xs.ElementAtOrNull(3);
    Console.WriteLine(v!.Y);
    Console.WriteLine(w = null);
    return 0;
  end Main;
end C;
)BZL"),
              "4\nTrue\n");
}

// A HEAP element type (a class), looped 100x: the materialized snapshot must
// not leak, and the borrowed class element must not be disposed by the
// terminal.
BEZEL_TEST(collection_elementatornull_8232, heap_element_loop_100_no_leak) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class Box
  public V: int;
  public constructor(v: int)
    V := v;
  end constructor;
  destructor end destructor;
end Box;
class C
  public static method Main(): int
    using var b1 := new Box(10);
    using var b2 := new Box(20);
    using var b3 := new Box(30);
    using var xs := new Array<Box>();
    xs.Add(b1); xs.Add(b2); xs.Add(b3);
    var t := 0;
    for var i in 0..<100 do
      var v := xs.ElementAtOrNull(1);
      if v <> null then t := t + v!.V; end if;
      var w := xs.ElementAtOrNull(7);
      if w = null then t := t + 1; end if;
      var u := xs.Where(lambda(b) => b.V > 10).ElementAtOrNull(0);
      if u <> null then t := t + u!.V; end if;
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
)BZL"),
                 "4100\n");
}

// stdlib section 15.1 / section 14.2: a member whose SIGNATURE forms `T?` at
// an inline-array element type is a conditionally-admitted member -- a
// compile error at the call, naming the member and the receiver.
//
// The row pins the §15.1 gate ALONE: at the base commit the call drew TWO
// diagnostics -- the "no member `ElementAtOrNull` on `Array<T>`" admission
// refusal (the defect) AND the §15.1 rejection (which the
// `typeLinqOperator` fallback reached anyway) -- so a row that only asserted
// the §15.1 text would have passed on the DEFECTIVE compiler. The
// "no member" count must be ZERO after the fix: the call is refused by the
// §15.1 register, and by nothing else.
BEZEL_TEST(collection_elementatornull_8232, inline_array_element_is_refused) {
    const JitResult r = runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class C
  public static method Main(): int
    using var xs := new Array<[3]int>();
    Console.WriteLine(xs.ElementAtOrNull(0) ?? -1);
    return 0;
  end Main;
end C;
)BZL");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "ElementAtOrNull"));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "nullable inline array"));
    BEZEL_EXPECT_EQ(bezel::test::diagnosticMessageCount(
                         r, "nullable inline array"), 1);
    // The admission refusal is GONE: the call is admitted by the name and
    // refused only by the §15.1 register.
    BEZEL_EXPECT_EQ(bezel::test::diagnosticMessageCount(r, "no member"), 0);
}

// `Dictionary<K, V>` (stdlib section 4.8) -- its element type is the
// synthetic `DictionaryEntry<K, V>` (the conformance is
// `ICollection<DictionaryEntry<K, V>>`), so section 5's `ElementAtOrNull`
// forms its `T?` at the ENTRY type: `DictionaryEntry<K, V>?`.
BEZEL_TEST(collection_elementatornull_8232, dictionary_receiver) {
    expectOut(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class C
  public static method Main(): int
    using var d := new Dictionary<int, int>();
    d.Add(1, 10); d.Add(2, 20); d.Add(3, 30);
    var e := d.ElementAtOrNull(1);
    Console.WriteLine(e!.Value);
    Console.WriteLine(e!.Key);
    Console.WriteLine(d.ElementAtOrNull(2)!.Value);
    Console.WriteLine(d.ElementAtOrNull(3) = null);
    Console.WriteLine(d.ElementAtOrNull(-1) = null);
    return 0;
  end Main;
end C;
)BZL"),
              "20\n2\n30\nTrue\nTrue\n");
}

// A STRING key: the isDict block rebuilds a fresh, independently-owned
// `bz_string*` Key for the entry it returns (stdlib section 4.8 / section
// 28.11) -- so the nullable form must carry that rebuild, and discharge it.
BEZEL_TEST(collection_elementatornull_8232, dictionary_string_key_receiver) {
    expectOut(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class C
  public static method Main(): int
    using var d := new Dictionary<string, int>();
    d.Add("a", 10); d.Add("bb", 20);
    var e := d.ElementAtOrNull(1);
    Console.WriteLine(e!.Key);
    Console.WriteLine(e!.Value);
    Console.WriteLine(d.ElementAtOrNull(5) = null);
    return 0;
  end Main;
end C;
)BZL"),
              "bb\n20\nTrue\n");
}

// A STRING value: the isDict block MINTS a +1 on the value it returns
// (stdlib section 28.11's "reference out" reading), so the nullable form
// must carry that mint, and discharge it.
BEZEL_TEST(collection_elementatornull_8232, dictionary_string_value_receiver) {
    expectOut(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class C
  public static method Main(): int
    using var d := new Dictionary<int, string>();
    d.Add(1, "aa"); d.Add(2, "bb");
    var e := d.ElementAtOrNull(1);
    Console.WriteLine(e!.Value);
    Console.WriteLine(e!.Key);
    Console.WriteLine(d.ElementAtOrNull(5) = null);
    return 0;
  end Main;
end C;
)BZL"),
              "bb\n2\nTrue\n");
}

// `SortedDictionary<K, V>` (stdlib section 4.17) -- the same element type,
// in ascending key order.
BEZEL_TEST(collection_elementatornull_8232, sorted_dictionary_receiver) {
    expectOut(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class C
  public static method Main(): int
    using var d := new SortedDictionary<int, int>();
    d.Add(3, 30); d.Add(1, 10); d.Add(2, 20);
    var e := d.ElementAtOrNull(0);
    Console.WriteLine(e!.Value);
    Console.WriteLine(d.ElementAtOrNull(2)!.Value);
    Console.WriteLine(d.ElementAtOrNull(3) = null);
    return 0;
  end Main;
end C;
)BZL"),
              "10\n30\nTrue\n");
}

// The dictionary receiver, looped 100x with a STRING key AND a STRING value:
// the rebuilt Key and the minted Value are the two ownership halves the
// nullable form has to carry, and neither may leak.
BEZEL_TEST(collection_elementatornull_8232, dictionary_loop_100_no_leak) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class C
  public static method Main(): int
    using var d := new Dictionary<string, string>();
    d.Add("a", "aa"); d.Add("bb", "bb"); d.Add("ccc", "ccc");
    var t := 0;
    for var i in 0..<100 do
      var e := d.ElementAtOrNull(1);
      if e <> null then t := t + e!.Value.Length; end if;
      var f := d.ElementAtOrNull(7);
      if f = null then t := t + 1; end if;
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
)BZL"),
                 "300\n");
}

// stdlib section 4.9: `OwningDictionary<K, V>`'s interface conformance is
// `IDisposable` only, so section 5's `IEnumerable<T>` extension -- and with it
// `ElementAtOrNull` -- has no applicable member on it.
BEZEL_TEST(collection_elementatornull_8232, owning_dictionary_is_refused) {
    const JitResult r = runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class C
  public static method Main(): int
    using var d := new OwningDictionary<int, int>();
    d.Add(1, 10);
    Console.WriteLine(d.ElementAtOrNull(0));
    return 0;
  end Main;
end C;
)BZL");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "does not expose `ElementAtOrNull`"));
}
