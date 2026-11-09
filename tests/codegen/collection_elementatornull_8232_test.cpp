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
using bezel::test::runAot;
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
//
// fix #8276 (stdlib section 4.11 "Iteration order" / section 5, v0.355): the
// pinned VALUE moved from 10 to 30.  A stack iterates in TAKE order -- top to
// bottom -- so section 5's `ElementAtOrNull(i)` (defined over the
// `IEnumerable<T>` iteration order) answers with the i-th element of THAT
// order: `ElementAtOrNull(0)` is the TOP (30), not the bottom (10).  The
// out-of-range row is unchanged: index 3 of a 3-element stack is still absent,
// so it is still null.
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
              "30\n-1\n");
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

// ---------------------------------------------------------------------------
// CONSUMERS of the nullable entry (fix-forward of the first review, which
// REJECTED 71569383). `d.ElementAtOrNull(i)` on a `Dictionary<K, V>` is a
// `DictionaryEntry<K, V>?`, laid out as the tagged `{i8 hasValue; {K, V}}`
// carrier (spec section 7.2). At 71569383 only `!`, `= null` and `??` read it
// correctly; every row below was MEASURED wrong there:
//
//   - a section 7.4 NARROWED read (`if e <> null then e.Key`) GEP'd the bare
//     `{K, V}` struct over the carrier and read the tag byte as the Key:
//     `e.Key + e.Value` printed 73732 for 3 + 5, a `string` Key segfaulted;
//   - section 7.5 `?.Key` / `?.Value` lowered to NOTHING (the #3079 / #2961
//     warnings: the `Console.WriteLine` printed nothing, the loop sum was 0);
//   - a WRITTEN `DictionaryEntry<K, V>?` parameter lowered to the `{i8, i32}`
//     fallback, so passing the entry failed IR verification.
// ---------------------------------------------------------------------------

// The review's r30: the section 7.4 narrowing idiom over an int/int entry.
BEZEL_TEST(collection_elementatornull_8232, dict_entry_narrowed_read_int_spec_7_4) {
    expectOut(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class C
  public static method Main(): int
    using var d := new Dictionary<int, int>();
    d.Add(3, 5);
    var e := d.ElementAtOrNull(0);
    if e <> null then Console.WriteLine(e.Key + e.Value); end if;
    var f := d.ElementAtOrNull(1);
    if f <> null then Console.WriteLine(f.Key); else Console.WriteLine("none"); end if;
    return 0;
  end Main;
end C;
)BZL"),
              "8\nnone\n");
}

// The review's r29: the same idiom over a `string` Key (segfaulted).
BEZEL_TEST(collection_elementatornull_8232, dict_entry_narrowed_read_string_key_spec_7_4) {
    expectOut(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class C
  public static method Main(): int
    using var d := new Dictionary<string, int>();
    d.Add("ka", 5);
    var e := d.ElementAtOrNull(0);
    if e <> null then Console.WriteLine(e.Key); Console.WriteLine(e.Value); end if;
    return 0;
  end Main;
end C;
)BZL"),
              "ka\n5\n");
}

// The review's r20: section 7.5 `?.Key` / `?.Value` on the UNBOUND call
// result, present and absent, string/string. `runAot`, not `runAotChecked`:
// an UNBOUND string-keyed entry temporary leaks its rebuilt Key and minted
// Value on every dictionary terminal. That is the pre-existing `ElementAt` twin
// (7000 B / 400 allocs for 200 `d.ElementAt(i).Key` reads at the base),
// tracked as #8267. The leak-free BOUND shapes are pinned under ASan below.
BEZEL_TEST(collection_elementatornull_8232, dict_entry_null_conditional_unbound_spec_7_5) {
    expectOut(runAot(R"BZL(
module M; import System; import System.Collections.Generic;
class C
  public static method Main(): int
    using var d := new Dictionary<string, string>();
    d.Add("ka", "va");
    Console.WriteLine(d.ElementAtOrNull(0)?.Key ?? "none");
    Console.WriteLine(d.ElementAtOrNull(1)?.Key ?? "none");
    var k := d.ElementAtOrNull(0)?.Value;
    Console.WriteLine(k ?? "none");
    return 0;
  end Main;
end C;
)BZL"),
              "ka\nnone\nva\n");
}

// The review's r23: a VALUE-typed member, so the section 7.5 result is an
// `int?` carrier, present and absent.
BEZEL_TEST(collection_elementatornull_8232, dict_entry_null_conditional_int_value_spec_7_5) {
    expectOut(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class C
  public static method Main(): int
    using var d := new Dictionary<int, int>();
    d.Add(1, 100);
    Console.WriteLine(d.ElementAtOrNull(0)?.Value ?? -1);
    Console.WriteLine(d.ElementAtOrNull(3)?.Value ?? -1);
    Console.WriteLine(d.ElementAtOrNull(0)?.Key ?? -1);
    return 0;
  end Main;
end C;
)BZL"),
              "100\n-1\n1\n");
}

// The review's r15: `?.Key` inside an expression, looped (keys "ka", "kb";
// index `i mod 3` is out of range one time in three). 200 iterations:
// 67 x 2 + 67 x 2 + 66 x 3 = 466. `runAot` for the #8267 reason above.
BEZEL_TEST(collection_elementatornull_8232, dict_entry_null_conditional_in_loop_spec_7_5) {
    expectOut(runAot(R"BZL(
module M; import System; import System.Collections.Generic;
class C
  public static method Main(): int
    using var d := new Dictionary<string, string>();
    d.Add("ka", "va"); d.Add("kb", "vb");
    var t := 0;
    for var i in 0..<200 do
      t := t + (d.ElementAtOrNull(i mod 3)?.Key ?? "xyz").Length;
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
)BZL"),
              "466\n");
}

// The review's r26: `?.` and the narrowed read on BOUND locals.
BEZEL_TEST(collection_elementatornull_8232, dict_entry_null_conditional_bound_spec_7_5) {
    expectOut(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class C
  public static method Main(): int
    using var d := new Dictionary<string, int>();
    d.Add("ka", 5);
    var e := d.ElementAtOrNull(0);
    var f := d.ElementAtOrNull(4);
    Console.WriteLine(e?.Value ?? -1);
    Console.WriteLine(f?.Value ?? -1);
    Console.WriteLine(e?.Key ?? "none");
    Console.WriteLine(f?.Key ?? "none");
    if e <> null then Console.WriteLine(e.Key); end if;
    return 0;
  end Main;
end C;
)BZL"),
              "5\n-1\nka\nnone\nka\n");
}

// The review's r27: the entry passed to a WRITTEN `DictionaryEntry<K, V>?`
// parameter (stdlib section 4.8 declares the record, so a user can name it),
// plus the section 5.1 widening of a non-nullable entry and a `null` literal.
// `runAot`: a string-keyed entry temporary passed BY VALUE is never released
// (#6153 / #8267). The non-nullable `Show(d.ElementAt(0))` twin leaks the
// same 35 B / 2 allocs at the base. The int/int row below pins the parameter
// path under ASan.
BEZEL_TEST(collection_elementatornull_8232, dict_entry_to_written_nullable_parameter) {
    expectOut(runAot(R"BZL(
module M; import System; import System.Collections.Generic;
class C
  static method Show(e: DictionaryEntry<string, int>?): string
    if e = null then return "null"; end if;
    return e!.Key + ":" + e!.Value.ToString();
  end Show;
  static method ShowNarrowed(e: DictionaryEntry<string, int>?): string
    if e <> null then return e.Key + "=" + e.Value.ToString(); end if;
    return "nil";
  end ShowNarrowed;
  public static method Main(): int
    using var d := new Dictionary<string, int>();
    d.Add("ka", 5);
    Console.WriteLine(Show(d.ElementAtOrNull(0)));
    Console.WriteLine(Show(d.ElementAtOrNull(1)));
    Console.WriteLine(ShowNarrowed(d.ElementAtOrNull(0)));
    Console.WriteLine(ShowNarrowed(d.ElementAtOrNull(1)));
    Console.WriteLine(Show(d.ElementAt(0)));
    Console.WriteLine(Show(null));
    return 0;
  end Main;
end C;
)BZL"),
              "ka:5\nnull\nka=5\nnil\nka:5\nnull\n");
}

// Every consumer above, looped 100x over a STRING key AND a STRING value
// (the rebuilt Key and the minted Value are the entry's two ownership
// halves): under runAotChecked's generated-code ASan nothing may leak.
BEZEL_TEST(collection_elementatornull_8232, dict_entry_consumers_loop_100_no_leak) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class C
  public static method Main(): int
    using var d := new Dictionary<string, string>();
    d.Add("a", "aa"); d.Add("bb", "bbb");
    var t := 0;
    for var i in 0..<100 do
      var e := d.ElementAtOrNull(i mod 3);
      if e <> null then t := t + e.Key.Length + e.Value.Length; end if;
      t := t + (e?.Key ?? "xyzw").Length;
      t := t + (e?.Value ?? "").Length;
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
)BZL"),
                 "666\n");
}

// The written-parameter and `?.` consumers on UNBOUND int/int entries,
// looped 100x under ASan (no heap field, so no #8267 leak to inherit).
// i mod 3 == 0 (34x): 2 * 10 + 10; == 1 (33x): 3 * 100 + 100;
// == 2 (33x, out of range): -1 + 1000 -> 1020 + 13200 + 32967 = 47187.
BEZEL_TEST(collection_elementatornull_8232, dict_entry_int_param_and_null_conditional_loop_100_no_leak) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class C
  static method Score(e: DictionaryEntry<int, int>?): int
    if e <> null then return e.Key * e.Value; end if;
    return -1;
  end Score;
  public static method Main(): int
    using var d := new Dictionary<int, int>();
    d.Add(2, 10); d.Add(3, 100);
    var t := 0;
    for var i in 0..<100 do
      t := t + Score(d.ElementAtOrNull(i mod 3));
      t := t + (d.ElementAtOrNull(i mod 3)?.Value ?? 1000);
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
)BZL"),
                 "47187\n");
}

// A member type that is ITSELF nullable (`Dictionary<int, int?>`): the
// section 7.5 result of `?.Value` is that same `int?`, null both when the
// entry is absent and when the stored value is null.
BEZEL_TEST(collection_elementatornull_8232, dict_entry_null_conditional_nullable_value_spec_7_5) {
    expectOut(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class C
  public static method Main(): int
    using var d := new Dictionary<int, int?>();
    d.Add(3, 5); d.Add(4, null);
    Console.WriteLine(d.ElementAtOrNull(0)?.Value ?? -1);
    Console.WriteLine(d.ElementAtOrNull(1)?.Value ?? -2);
    Console.WriteLine(d.ElementAtOrNull(2)?.Value ?? -3);
    var e := d.ElementAtOrNull(0);
    if e <> null then Console.WriteLine(e.Value ?? -4); end if;
    return 0;
  end Main;
end C;
)BZL"),
              "5\n-2\n-3\n5\n");
}
