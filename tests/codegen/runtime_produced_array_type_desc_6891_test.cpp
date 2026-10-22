// Spec-derived regression test for issue #6891.
//
// Spec §8.5 (v0.340): an Object-typed value's ToString() with no override is
// the type's qualified name, so an Array<string> prints
// "System.Collections.Generic.Array<string>". Spec §12.7 (v0.340): `is` tests
// the runtime type, so `o is Array<string>` is True and `o is Array<int>` is
// False for a string array. stdlib §9.6 (`String.Split`), §5.x (lazy
// `ToArray`, `Distinct`) and File.ReadAllLines (stdlib §7, `owned
// Array<string>`) return Array<T>.
//
// THE DEFECT: arrays the RUNTIME allocates carried no per-instantiation
// descriptor (word 1 of bz_array_t, #6776), so through an Object they rendered
// "System.Collections.Generic.Array" and `o is Array<string>` was False.
// Codegen now stamps the descriptor at the call site (stampRuntimeProducedArray).
//
// Every row compares exact stdout under the checked oracle (ASan + LSan);
// the arrays are passed to Show through a local binding the callee borrows,
// so each iteration's array is disposed by its owner.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::ModuleSource;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotModulesChecked;

namespace {

std::string out(const bezel::test::JitResult& r) {
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + "> " + r.stdoutText;
    return r.stdoutText;
}

std::string prog(const std::string& body) {
    return "module M; class C "
           "  public static method Show(o: Object) "
           "    Console.WriteLine(o); Console.WriteLine(o is Array<string>); "
           "    Console.WriteLine(o is Array<int>); "
           "  end Show; "
           "  public static method Main(): int " + body +
           "    return 0; end Main; end C;";
}

const char* kStr = "System.Collections.Generic.Array<string>\nTrue\nFalse\n";
const char* kInt = "System.Collections.Generic.Array<int>\nFalse\nTrue\n";

}  // namespace

BEZEL_TEST(codegen_runtime_produced_array_type_desc_6891, string_split_8_5_12_7) {
    BEZEL_EXPECT_EQ(out(runAotChecked(prog(
        "var a := \"a,b\".Split(\",\"); Show(a);"))), std::string(kStr));
}

BEZEL_TEST(codegen_runtime_produced_array_type_desc_6891, string_split_char_8_5_12_7) {
    BEZEL_EXPECT_EQ(out(runAotChecked(prog(
        "var a := \"a,b\".Split(','); Show(a);"))), std::string(kStr));
}

BEZEL_TEST(codegen_runtime_produced_array_type_desc_6891, where_select_toarray_8_5_12_7) {
    BEZEL_EXPECT_EQ(out(runAotChecked(prog(
        "var xs: Array<int> := [1, 2, 3, 2]; "
        "var a := xs.Where(lambda(x: int) => x > 1).ToArray(); Show(a); "
        "var b := xs.Select(lambda(x: int) => \"s\").ToArray(); Show(b);"))),
        std::string(kInt) + kStr);
}

BEZEL_TEST(codegen_runtime_produced_array_type_desc_6891, distinct_toarray_8_5_12_7) {
    BEZEL_EXPECT_EQ(out(runAotChecked(prog(
        "var xs: Array<int> := [1, 2, 3, 2]; "
        "var a := xs.Distinct().ToArray(); Show(a);"))), std::string(kInt));
}

// Plain oracle: `File.*` leaks its 54-byte NUL-terminated path copy
// (strToPath's malloc) on the base too, independent of this issue, which would
// fail the checked oracle for an unrelated reason.
BEZEL_TEST(codegen_runtime_produced_array_type_desc_6891, readalllines_8_5_12_7) {
    BEZEL_EXPECT_EQ(out(runAot(prog(
        "var p := \"/var/tmp/bezel-scratch/r34-6891/rows_readalllines.txt\"; "
        "File.WriteAllLines(p, [\"p\", \"q\"]); "
        "var a := File.ReadAllLines(p); Show(a); File.Delete(p);"))),
        std::string(kStr));
}

// A looped row so a per-iteration leak from the extra store is visible.
BEZEL_TEST(codegen_runtime_produced_array_type_desc_6891, split_looped_no_leak) {
    std::string expect;
    for (int i = 0; i < 100; ++i) expect += kStr;
    BEZEL_EXPECT_EQ(out(runAotChecked(prog(
        "var i := 0; while i < 100 do "
        "var a := \"a,b\".Split(\",\"); Show(a); i := i + 1; end while;"))), expect);
}

// Negative: a string array is not an Array<int> (and vice versa) -- covered
// by the False lines above; this row pins the typed (non-Object) path.
BEZEL_TEST(codegen_runtime_produced_array_type_desc_6891, static_path_unchanged) {
    BEZEL_EXPECT_EQ(out(runAotChecked(
        "module M; class C public static method Main(): int "
        "  var a := \"a,b,c\".Split(\",\"); Console.WriteLine(a.Count); "
        "  Console.WriteLine(a[2]); return 0; end Main; end C;")),
        std::string("3\nc\n"));
}

// Cross-module: the producer runs in the library and is returned as
// Array<string>; the importer sees it through Object.
BEZEL_TEST(codegen_runtime_produced_array_type_desc_6891, cross_module_returned_split) {
    const char* lib =
        "module Lib; public class P "
        "  public static method Parts(s: string): owned Array<string> "
        "    return s.Split(\",\"); end Parts; end P;";
    const char* app =
        "module App; import Lib; public class Program "
        "  public static method Show(o: Object) "
        "    Console.WriteLine(o); Console.WriteLine(o is Array<string>); "
        "    Console.WriteLine(o is Array<int>); end Show; "
        "  public static method Main(): int "
        "    var a := P.Parts(\"a,b\"); Show(a); return 0; end Main; "
        "end Program;";
    BEZEL_EXPECT_EQ(out(runAotModulesChecked({ModuleSource{"Lib", lib},
                                              ModuleSource{"App", app}}, "App")),
                    std::string(kStr));
}
