// Codegen / disposal — a GENERIC collection's element slot never took or
// released the +1 per stored string element, because the string-element
// predicate read the collection's `genericArgs[0]` — inside a monomorphized
// generic body that is still the PLACEHOLDER type parameter `T` — instead of
// the SUBSTITUTED element type. §14.1/§14.4 fix the element category at the
// INSTANTIATION, not at the generic declaration. The DISPOSAL side resolves the
// element type, so the pair was UNPAIRED: the slot took no +1 while the
// collection's free gave one back, and an element whose owner had died was read
// after it was freed (heap-use-after-free). Issue #9100.
//
// Spec: §14.1 "Generic types and type parameters" (monomorphization keeps the
// declaration's spelling; the substitution binds the concrete type),
// §14.4 "Instantiation" (the element category is fixed at the instantiation),
// §28.9 "Runtime-managed types" (a `string` is refcounted; `null` is a
// retain/release no-op), §28.11 "Collection element ownership" *The element
// slot policy* (the runtime-managed-reference row's borrowing-family column is
// "shared references": the collection owns a +1 of its own on the element and
// releases it when the slot goes away). Spec revision v0.359.
//
// Model: #9064 (the non-generic `Array<string?>` half of the same pair — a
// `string?` element is the same §28.11 row as a `string` one, §3.6 reads the
// element category of a nullable `V?` as `V`).
//
// Every string is built at run time (a folded literal is an RC sentinel on
// which release is a no-op and would hide the leak). runAotChecked runs under
// ASan/LSan, so both the leak (the +1 the slot never took) and the
// use-after-free / double-free flip `r.ran`; stdout pins the values (a
// use-after-free on read-back would corrupt them).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// The issue's own repro: a generic `Box<T>` with an OWNED `Array<T>` field,
// instantiated at `T = string?`. The producer is built in the CALLER and
// handed to `Put` as a parameter, so the slot must take a +1 of its own — the
// caller's temporary is released at the end of the statement and the slot's
// element would dangle without it.
//
// "w0".."w199" are 2, 3 or 4 characters -> 10*2 + 90*3 + 100*4 = 690.
const char* kGenericBoxNullableString =
    "module M; import System;\n"
    "public class Box<T>\n"
    "  public owned items: Array<T>;\n"
    "  public constructor() items := new Array<T>(); end;\n"
    "  public method Put(v: T) items.Add(v); end Put;\n"
    "end Box;\n"
    "public class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    for var i in 0..<200 do\n"
    "      var b := new Box<string?>();\n"
    "      b.Put(\"w\" + i.ToString());\n"
    "      b.Put(null);\n"
    "      for var x in b.items do tot := tot + (x ?? \"\").Length; end;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// The same generic instantiation at a NON-NULLABLE element type: the element
// category is the same §28.11 row, so the same +1 must be taken and given
// back. This row isolates the SUBSTITUTION half of the fix (it does not
// depend on a nullable element at all).
const char* kGenericBoxString =
    "module M; import System;\n"
    "public class Box<T>\n"
    "  public owned items: Array<T>;\n"
    "  public constructor() items := new Array<T>(); end;\n"
    "  public method Put(v: T) items.Add(v); end Put;\n"
    "end Box;\n"
    "public class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    for var i in 0..<200 do\n"
    "      var b := new Box<string>();\n"
    "      b.Put(\"w\" + i.ToString());\n"
    "      for var x in b.items do tot := tot + x.Length; end;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// Control: the non-generic twin is unchanged — it already took and released
// the +1 per element.
const char* kControlNonNullableArray =
    "module M; import System;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    var i := 0;\n"
    "    while i < 200 do\n"
    "      var a := new Array<string>();\n"
    "      a.Add(\"w\" + (40 + i mod 10).ToString());\n"
    "      tot := tot + a[0].Length;\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// Control: a NULL element is the null-safe retain/release no-op — it takes no
// reference and gives none back.
const char* kControlNullElement =
    "module M; import System;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    var i := 0;\n"
    "    while i < 200 do\n"
    "      var a := new Array<string?>();\n"
    "      a.Add(null);\n"
    "      a.Add(null);\n"
    "      tot := tot + a.Count;\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// ---- fix-forward #9100 (v0.359): the take/release split the first #9100
// commit left. #9100 made every RELEASE half of an `Array<T>` read the
// SUBSTITUTED element type but left TAKE halves (the LINQ `ToArray` retain)
// on the placeholder `T`; #9107 made the borrowing LINKED LIST co-own a
// `string?` element, but its generic inserts still read `T` raw while the
// caller's concrete list released; and the owned-field free resolved `T` for
// a Stack / Queue field whose generic Push / Enqueue does not. Each row below
// is the measured repro of one side of that split (red on the #9100 commit).

// x8: `using var r := src.ToArray()` in a generic body at T = `string` — the
// copy took no +1 per element while the `using var` free of the `Array<T>`
// result released one: the caller's elements were freed under it
// (heap-use-after-free on the read-back).
// "w0".."w199" + "v0".."v199" -> 2 * 690 = 1380, plus Count 2 * 200 = 400.
const char* kGenericToArrayUsingString =
    "module M; import System;\n"
    "public class G\n"
    "  public static method Snap<T>(src: Array<T>): int\n"
    "    using var r := src.ToArray(); return r.Count;\n"
    "  end Snap;\n"
    "end G;\n"
    "public class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    for var i in 0..<200 do\n"
    "      var a := new Array<string>();\n"
    "      a.Add(\"w\" + i.ToString()); a.Add(\"v\" + i.ToString());\n"
    "      tot := tot + G.Snap<string>(a);\n"
    "      for var x in a do tot := tot + x.Length; end;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// x8 at T = `string?` (§3.6 *Nullable forms*: the same §28.11 row).
const char* kGenericToArrayUsingNullableString =
    "module M; import System;\n"
    "public class G\n"
    "  public static method Snap<T>(src: Array<T>): int\n"
    "    using var r := src.ToArray(); return r.Count;\n"
    "  end Snap;\n"
    "end G;\n"
    "public class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    for var i in 0..<200 do\n"
    "      var a := new Array<string?>();\n"
    "      a.Add(\"w\" + i.ToString()); a.Add(\"v\" + i.ToString());\n"
    "      tot := tot + G.Snap<string?>(a);\n"
    "      for var x in a do tot := tot + (x ?? \"\").Length; end;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// x10: the same through a LINQ pipeline, `src.Where(...).ToArray()`.
const char* kGenericWhereToArrayUsingString =
    "module M; import System;\n"
    "public class G\n"
    "  public static method Snap<T>(src: Array<T>): int\n"
    "    using var r := src.Where(lambda(x) => true).ToArray();\n"
    "    return r.Count;\n"
    "  end Snap;\n"
    "end G;\n"
    "public class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    for var i in 0..<200 do\n"
    "      var a := new Array<string>();\n"
    "      a.Add(\"w\" + i.ToString()); a.Add(\"v\" + i.ToString());\n"
    "      tot := tot + G.Snap<string>(a);\n"
    "      for var x in a do tot := tot + x.Length; end;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// A generic body RETURNING the `ToArray` copy to a caller whose concrete
// `Array<string>` free releases a +1 per element — the take must be in the
// generic body ("refcount over-release (string)" on the #9100 commit).
const char* kGenericToArrayReturnedString =
    "module M; import System;\n"
    "public class G\n"
    "  public static method Snap<T>(src: Array<T>): owned Array<T>\n"
    "    return src.ToArray();\n"
    "  end Snap;\n"
    "end G;\n"
    "public class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    for var i in 0..<200 do\n"
    "      var a := new Array<string>();\n"
    "      a.Add(\"w\" + i.ToString()); a.Add(\"v\" + i.ToString());\n"
    "      using var r := G.Snap<string>(a);\n"
    "      tot := tot + r.Count;\n"
    "      for var x in a do tot := tot + x.Length; end;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// t06_generic_n: a generic `Box<T>` with an owned `LinkedList<T>` field at
// T = `string?` — the generic AddLast / AddFirst took nothing while the
// field's free (which resolves `T`) released one per element.
// Count 3 per iteration -> 600.
const char* kGenericLinkedListFieldNullableString =
    "module M; import System;\n"
    "public class Box<T>\n"
    "  private owned _items: LinkedList<T> := new LinkedList<T>();\n"
    "  public method Put(x: T) _items.AddLast(x); end Put;\n"
    "  public method PutFirst(x: T) _items.AddFirst(x); end PutFirst;\n"
    "  public method Cnt(): int return _items.Count; end Cnt;\n"
    "end Box;\n"
    "public class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    for var i in 0..<200 do\n"
    "      var b := new Box<string?>();\n"
    "      b.Put(\"w\" + i.ToString());\n"
    "      b.PutFirst(null);\n"
    "      b.Put(\"v\" + i.ToString());\n"
    "      tot := tot + b.Cnt();\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// t07_generic_local_n: a generic body inserting into a CONCRETE
// `LinkedList<string?>` the caller owns — the caller's free releases a +1
// per element, so the generic insert must take one. Count 2 -> 400.
const char* kGenericFillCallersLinkedListNullableString =
    "module M; import System;\n"
    "public class C\n"
    "  public static method Fill<T>(l: LinkedList<T>, a: T, b: T)\n"
    "    l.AddLast(a); l.AddFirst(b);\n"
    "  end Fill;\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    for var i in 0..<200 do\n"
    "      var l := new LinkedList<string?>();\n"
    "      C.Fill<string?>(l, \"g\" + i.ToString(), null);\n"
    "      tot := tot + l.Count;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// The Stack / Queue twins (y_Stack_sn_livesrc / y_Queue_sn_livesrc): a
// generic `Box<T>` with an owned `Stack<T>` / `Queue<T>` field at T =
// `string?`, fed a LIVE caller local twice. The generic Push / Enqueue
// keeps the declaration-site reading (the Stack / Queue Pop / Dequeue does
// not release its slot's reference — #8453 / #9136), so the field's free
// must read it too; releasing at the resolved type gave the caller's string
// back twice ("refcount over-release (string)"). Count 2 -> 400.
const char* kGenericStackFieldLiveSourceNullableString =
    "module M; import System;\n"
    "public class Box<T>\n"
    "  public owned items: Stack<T>;\n"
    "  public constructor() items := new Stack<T>(); end;\n"
    "  public method Put(v: T) items.Push(v); end Put;\n"
    "end Box;\n"
    "public class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    for var i in 0..<200 do\n"
    "      var s: string? := \"w\" + i.ToString();\n"
    "      var b := new Box<string?>();\n"
    "      b.Put(s); b.Put(s);\n"
    "      tot := tot + b.items.Count;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

const char* kGenericQueueFieldLiveSourceNullableString =
    "module M; import System;\n"
    "public class Box<T>\n"
    "  public owned items: Queue<T>;\n"
    "  public constructor() items := new Queue<T>(); end;\n"
    "  public method Put(v: T) items.Enqueue(v); end Put;\n"
    "end Box;\n"
    "public class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    for var i in 0..<200 do\n"
    "      var s: string? := \"w\" + i.ToString();\n"
    "      var b := new Box<string?>();\n"
    "      b.Put(s); b.Put(s);\n"
    "      tot := tot + b.items.Count;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// Drop-on-overwrite of an `owned` collection field: the DISPLACED
// collection's release reads the element type as the holder's disposal does —
// no `!nullable` gate (§3.6 *Nullable forms*) and the SUBSTITUTED type in a
// generic body. Both leaked one reference per displaced element on the
// #9100 commit. Count 1 per iteration -> 200.
const char* kOverwriteNullableStringArrayField =
    "module M; import System;\n"
    "public class H\n"
    "  public owned items: Array<string?> := new Array<string?>();\n"
    "  public method Reset() items := new Array<string?>(); end Reset;\n"
    "end H;\n"
    "public class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    for var i in 0..<200 do\n"
    "      using var h := new H();\n"
    "      h.items.Add(\"w\" + i.ToString()); h.items.Add(null);\n"
    "      h.Reset();\n"
    "      h.items.Add(\"v\" + i.ToString());\n"
    "      tot := tot + h.items.Count;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

const char* kOverwriteGenericStringArrayField =
    "module M; import System;\n"
    "public class Box<T>\n"
    "  public owned items: Array<T> := new Array<T>();\n"
    "  public method Put(v: T) items.Add(v); end Put;\n"
    "  public method Reset() items := new Array<T>(); end Reset;\n"
    "end Box;\n"
    "public class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    for var i in 0..<200 do\n"
    "      using var b := new Box<string>();\n"
    "      b.Put(\"w\" + i.ToString()); b.Put(\"v\" + i.ToString());\n"
    "      b.Reset();\n"
    "      b.Put(\"u\" + i.ToString());\n"
    "      tot := tot + b.items.Count;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// An unbound `Array<string?>` TEMPORARY (`a.ToArray().Count`): the copy
// takes a +1 per element for a `string?` as for a `string` (§3.6 *Nullable
// forms*), so the temporary's reclaim must give it back — it was still
// `!nullable`-gated and leaked one reference per element.
const char* kConcreteNullableStringToArrayTemporary =
    "module M; import System;\n"
    "public class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    for var i in 0..<200 do\n"
    "      var a := new Array<string?>();\n"
    "      a.Add(\"w\" + i.ToString()); a.Add(\"v\" + i.ToString());\n"
    "      tot := tot + a.ToArray().Count;\n"
    "      for var x in a do tot := tot + (x ?? \"\").Length; end;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// A generic body's `using var f := FrozenArray<T>.From(src)` at T = `string`:
// the copying `From` takes the +1 at the SUBSTITUTED type, so the local's
// scope-end free must release at it too (it read `T` raw and leaked).
const char* kGenericFrozenArrayLocalString =
    "module M; import System;\n"
    "public class G\n"
    "  public static method Cnt<T>(src: Array<T>): int\n"
    "    using var f := FrozenArray<T>.From(src);\n"
    "    return f.Length;\n"
    "  end Cnt;\n"
    "end G;\n"
    "public class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    for var i in 0..<200 do\n"
    "      var a := new Array<string>();\n"
    "      a.Add(\"w\" + i.ToString()); a.Add(\"v\" + i.ToString());\n"
    "      tot := tot + G.Cnt<string>(a);\n"
    "      for var x in a do tot := tot + x.Length; end;\n"
    "    end;\n"
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

BEZEL_TEST(ownership_generic_collection_string_element_substitution_9100,
           generic_box_nullable_string_element_is_co_owned_spec_28_11) {
    expectClean(kGenericBoxNullableString, "690\n");
}

BEZEL_TEST(ownership_generic_collection_string_element_substitution_9100,
           generic_box_string_element_is_co_owned_spec_28_11) {
    expectClean(kGenericBoxString, "690\n");
}

BEZEL_TEST(ownership_generic_collection_string_element_substitution_9100,
           control_nonnullable_array_unchanged_spec_28_9) {
    expectClean(kControlNonNullableArray, "600\n");
}

BEZEL_TEST(ownership_generic_collection_string_element_substitution_9100,
           control_null_element_is_a_no_op_spec_28_9) {
    expectClean(kControlNullElement, "400\n");
}

// ---- fix-forward #9100 rows.

BEZEL_TEST(ownership_generic_collection_string_element_substitution_9100,
           generic_toarray_using_string_takes_per_element_spec_28_11) {
    expectClean(kGenericToArrayUsingString, "1780\n");
}

BEZEL_TEST(ownership_generic_collection_string_element_substitution_9100,
           generic_toarray_using_nullable_string_takes_per_element_spec_28_11) {
    expectClean(kGenericToArrayUsingNullableString, "1780\n");
}

BEZEL_TEST(ownership_generic_collection_string_element_substitution_9100,
           generic_where_toarray_using_string_takes_per_element_spec_28_11) {
    expectClean(kGenericWhereToArrayUsingString, "1780\n");
}

BEZEL_TEST(ownership_generic_collection_string_element_substitution_9100,
           generic_toarray_returned_string_takes_per_element_spec_28_11) {
    expectClean(kGenericToArrayReturnedString, "1780\n");
}

BEZEL_TEST(ownership_generic_collection_string_element_substitution_9100,
           generic_linkedlist_field_nullable_string_insert_takes_spec_28_11) {
    expectClean(kGenericLinkedListFieldNullableString, "600\n");
}

BEZEL_TEST(ownership_generic_collection_string_element_substitution_9100,
           generic_fill_callers_linkedlist_nullable_string_takes_spec_28_11) {
    expectClean(kGenericFillCallersLinkedListNullableString, "400\n");
}

BEZEL_TEST(ownership_generic_collection_string_element_substitution_9100,
           generic_stack_field_live_source_released_as_taken_spec_28_11) {
    expectClean(kGenericStackFieldLiveSourceNullableString, "400\n");
}

BEZEL_TEST(ownership_generic_collection_string_element_substitution_9100,
           generic_queue_field_live_source_released_as_taken_spec_28_11) {
    expectClean(kGenericQueueFieldLiveSourceNullableString, "400\n");
}

BEZEL_TEST(ownership_generic_collection_string_element_substitution_9100,
           overwrite_nullable_string_array_field_releases_displaced_spec_28_11) {
    expectClean(kOverwriteNullableStringArrayField, "200\n");
}

BEZEL_TEST(ownership_generic_collection_string_element_substitution_9100,
           overwrite_generic_string_array_field_releases_displaced_spec_28_11) {
    expectClean(kOverwriteGenericStringArrayField, "200\n");
}

BEZEL_TEST(ownership_generic_collection_string_element_substitution_9100,
           nullable_string_toarray_temporary_reclaim_releases_spec_28_11) {
    expectClean(kConcreteNullableStringToArrayTemporary, "1780\n");
}

BEZEL_TEST(ownership_generic_collection_string_element_substitution_9100,
           generic_frozenarray_local_string_released_as_taken_spec_28_11) {
    expectClean(kGenericFrozenArrayLocalString, "1780\n");
}
