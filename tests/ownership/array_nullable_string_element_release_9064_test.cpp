// Codegen / disposal — a borrowing `Array<string?>` never took or released the
// +1 per stored string element that `Array<string>` does (every string-element
// predicate carried a `!nullable` gate), so a +1 string PRODUCER passed to
// `Array<string?>.Add` was orphaned — neither retained by the slot nor
// transferred into it — and leaked; and a BORROWED string passed to the same
// call was stored without a reference, so it died with its source (a
// use-after-free). Issue #9064.
//
// Spec: §3.6 "Derived type properties" *Nullable forms* (the ELEMENT CATEGORY
// of a nullable type `V?` is `V`'s, nullability stripped), §28.9 "Runtime-
// managed types" (a `string` is refcounted; null is a retain/release no-op),
// §28.11 "Collection element ownership" *The element slot policy* (the
// runtime-managed-reference row's borrowing-family column is "shared
// references": the collection owns a +1 of its own on the element and releases
// it when the slot goes away). Spec revision v0.359.
//
// Model: #8207 (the `Dictionary<K, string?>` twin of the same gate), which
// named this Array position as the remaining sibling.
//
// Every string is built at run time (a folded literal is an RC sentinel on
// which release is a no-op and would hide the leak). runAotChecked runs under
// ASan/LSan, so both a leak (the bug) and a double-free / use-after-free flip
// `r.ran`; stdout pins the values (a use-after-free on read-back would
// corrupt them).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// The issue's own repro: a +1 string producer passed to `Array<string?>.Add`.
// "w4x" has length 3 -> 3 * 200 = 600.
const char* kAddProducer =
    "module M; import System;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    var i := 0;\n"
    "    while i < 200 do\n"
    "      var a := new Array<string?>();\n"
    "      a.Add(\"w\" + (40 + i mod 10).ToString());\n"
    "      tot := tot + (a[0] ?? \"\").Length;\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// The same producer stored and then dropped by `Clear()`: the slot's +1 must
// be given back when the slot goes away, not only at the array's free.
const char* kAddProducerThenClear =
    "module M; import System;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    var i := 0;\n"
    "    while i < 200 do\n"
    "      var a := new Array<string?>();\n"
    "      a.Add(\"w\" + (40 + i mod 10).ToString());\n"
    "      a.Clear();\n"
    "      tot := tot + a.Count;\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// The BORROWED-argument twin (#8485): a string read out of another array is a
// borrow; storing it must take a reference of its own, or it dies with its
// source and every later read is a use-after-free.
const char* kAddBorrowed =
    "module M; import System;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var ss := new Array<string?>();\n"
    "    for var i in 1..<3 do\n"
    "      var f := $\"name{i},z\".Split(',');\n"
    "      ss.Add(f[0]);\n"
    "    end;\n"
    "    for var s in ss do Console.WriteLine(s ?? \"null\"); end;\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// Control: the non-nullable twin is unchanged (it already took and released
// the +1 per element).
const char* kControlNonNullable =
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

BEZEL_TEST(ownership_array_nullable_string_element_release_9064,
           add_producer_element_is_taken_spec_28_11) {
    expectClean(kAddProducer, "600\n");
}

BEZEL_TEST(ownership_array_nullable_string_element_release_9064,
           clear_returns_the_element_reference_spec_28_11) {
    expectClean(kAddProducerThenClear, "0\n");
}

BEZEL_TEST(ownership_array_nullable_string_element_release_9064,
           add_borrowed_element_takes_its_own_reference_spec_28_11) {
    expectClean(kAddBorrowed, "name1\nname2\n");
}

BEZEL_TEST(ownership_array_nullable_string_element_release_9064,
           control_nonnullable_array_unchanged_spec_28_9) {
    expectClean(kControlNonNullable, "600\n");
}

BEZEL_TEST(ownership_array_nullable_string_element_release_9064,
           control_null_element_is_a_no_op_spec_28_9) {
    expectClean(kControlNullElement, "400\n");
}
