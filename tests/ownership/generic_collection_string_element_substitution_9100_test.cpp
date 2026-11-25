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
