// Codegen / disposal — a borrowing `Dictionary<K, string?>` never took or
// released the +1 per stored string value that `Dictionary<K, string>` does
// (every `dictIsStringValue` / `isStrVal` / scope-end-disposer predicate carried
// a `!nullable` gate), so a runtime-built string stored through the indexer
// set, `.Add` or `GetOrAdd`'s factory leaked. Issue #8207.
//
// Spec: §28.9 "Runtime-managed types" (a `string` is refcounted and a nullable
// `string?` is the same runtime-managed row; null is a retain/release no-op),
// §28.11 (a borrowing Dictionary<K, string> co-owns a +1 per stored value),
// stdlib §4.8 Dictionary<K, V>.GetOrAdd. Spec revision v0.355.
//
// Every string is built at run time (a folded literal is an RC sentinel on
// which release is a no-op and would hide the leak). runAotChecked runs under
// ASan/LSan, so both a leak (the bug) and a double-free flip `r.ran`; stdout
// pins the values (a use-after-free on read-back would corrupt them).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// Issue repro, 200 iterations. "w4x" has length 3 -> 600.
const char* kGetOrAddMiss =
    "module M; import System;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    var i := 0;\n"
    "    while i < 200 do\n"
    "      var d := new Dictionary<int, string?>();\n"
    "      var s := d.GetOrAdd(i, lambda(k: int): string? => \"w\" + (40 + k mod 10).ToString());\n"
    "      tot := tot + (s ?? \"\").Length;\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// Hit path: the factory runs once; the second GetOrAdd returns the stored value.
const char* kGetOrAddHit =
    "module M; import System;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    var i := 0;\n"
    "    while i < 200 do\n"
    "      var d := new Dictionary<int, string?>();\n"
    "      var a := d.GetOrAdd(7, lambda(k: int): string? => \"h\" + (40 + k).ToString());\n"
    "      var b := d.GetOrAdd(7, lambda(k: int): string? => \"zz\" + k.ToString());\n"
    "      tot := tot + (a ?? \"\").Length + (b ?? \"\").Length;\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// Indexer set over a new key. "w4x" -> 3 * 200 = 600.
const char* kIndexerSetNew =
    "module M; import System;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    var i := 0;\n"
    "    while i < 200 do\n"
    "      var d := new Dictionary<int, string?>();\n"
    "      d[i] := \"w\" + (40 + i mod 10).ToString();\n"
    "      tot := tot + (d[i] ?? \"\").Length;\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// Indexer overwrite of an existing key releases the OLD value; the value read
// back is the NEW string (no use-after-free). "b4x" -> 3 * 200 = 600.
const char* kIndexerOverwrite =
    "module M; import System;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    var i := 0;\n"
    "    while i < 200 do\n"
    "      var d := new Dictionary<int, string?>();\n"
    "      d[1] := \"a\" + (40 + i mod 10).ToString();\n"
    "      d[1] := \"b\" + (40 + i mod 10).ToString();\n"
    "      tot := tot + (d[1] ?? \"\").Length;\n"
    "      if i = 199 then Console.WriteLine(d[1] ?? \"null\"); end;\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// Add, Remove, Clear, and a null stored then overwritten.
const char* kAddRemoveClearNull =
    "module M; import System;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    var i := 0;\n"
    "    while i < 200 do\n"
    "      var d := new Dictionary<int, string?>();\n"
    "      d.Add(1, \"q\" + (40 + i mod 10).ToString());\n"
    "      d.Add(2, \"r\" + (40 + i mod 10).ToString());\n"
    "      d.Remove(1);\n"
    "      d.Clear();\n"
    "      d[3] := null;\n"
    "      d[3] := \"y\" + (40 + i mod 10).ToString();\n"
    "      d[4] := \"x\" + (40 + i mod 10).ToString();\n"
    "      d[4] := null;\n"
    "      tot := tot + (d[3] ?? \"\").Length + (d[4] ?? \"\").Length + d.Count;\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// A BORROWED string shared by two dictionaries is retained by each (not a
// +1 producer): no double-free, no leak. Self-overwrite keeps it alive.
const char* kSharedBorrowedString =
    "module M; import System;\n"
    "class C\n"
    "  public static method Two(s: string): int\n"
    "    var d1 := new Dictionary<int, string?>();\n"
    "    var d2 := new Dictionary<int, string?>();\n"
    "    d1[1] := s;\n"
    "    d2[1] := s;\n"
    "    d1[1] := d1[1];\n"
    "    return (d1[1] ?? \"\").Length + (d2[1] ?? \"\").Length;\n"
    "  end Two;\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    var i := 0;\n"
    "    while i < 200 do\n"
    "      tot := tot + C.Two(\"v-\" + (100 + i).ToString());\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// Control: the non-nullable twin stays clean and unchanged.
const char* kControlNonNullable =
    "module M; import System;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    var i := 0;\n"
    "    while i < 200 do\n"
    "      var d := new Dictionary<int, string>();\n"
    "      var s := d.GetOrAdd(i, lambda(k: int): string => \"w\" + (40 + k mod 10).ToString());\n"
    "      d[1] := \"a\" + (40 + i mod 10).ToString();\n"
    "      d[1] := \"b\" + (40 + i mod 10).ToString();\n"
    "      tot := tot + s.Length + d[1].Length;\n"
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

BEZEL_TEST(codegen_dict_nullable_string_value_release_8207,
           getoradd_miss_releases_string_value_spec_28_9) {
    expectClean(kGetOrAddMiss, "600\n");
}

BEZEL_TEST(codegen_dict_nullable_string_value_release_8207,
           getoradd_hit_returns_stored_value_spec_28_9) {
    expectClean(kGetOrAddHit, "1200\n");  // "h47" (3) read twice * 200
}

BEZEL_TEST(codegen_dict_nullable_string_value_release_8207,
           indexer_set_new_key_releases_string_value_spec_28_9) {
    expectClean(kIndexerSetNew, "600\n");
}

BEZEL_TEST(codegen_dict_nullable_string_value_release_8207,
           indexer_overwrite_releases_old_value_reads_new_spec_28_9) {
    expectClean(kIndexerOverwrite, "b49\n600\n");
}

BEZEL_TEST(codegen_dict_nullable_string_value_release_8207,
           add_remove_clear_and_null_overwrite_balanced_spec_28_9) {
    expectClean(kAddRemoveClearNull, "1000\n");  // (3 + 0 + Count 2) * 200
}

BEZEL_TEST(codegen_dict_nullable_string_value_release_8207,
           shared_borrowed_string_retained_per_dictionary_spec_28_11) {
    expectClean(kSharedBorrowedString, "2000\n");  // 2 * len("v-NNN")=5 * 200
}

BEZEL_TEST(codegen_dict_nullable_string_value_release_8207,
           control_nonnullable_dictionary_unchanged_spec_28_9) {
    expectClean(kControlNonNullable, "1200\n");
}
