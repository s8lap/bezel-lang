// Regression test for issue #6898 (§27.6 / §26.5 / §26.4 / §2.8.2, v0.340).
//
// A method reference to an extension method declared in an IMPORTED module
// evaluated to a zero-initialized value and the program then segfaulted
// (rc 139), where the same reference to a same-module extension prints 5.
// Two causes, one per row group:
//
//  * `public extension IList<int>` (a GROUND generic head, no free type
//    variable) was dropped from the exported metadata, so the importer found
//    no extension at all. A bare reference `il.Foo` then degraded to a zero
//    slot (warning #2434) and the call `il.Foo(7)` was rejected. The exporter
//    now carries a ground generic head, and the importer spells it by its bare
//    head name, the name the exporting side mangles `bz$ext$<M>.IList$Foo`
//    with.
//  * the type of a bound reference to an imported extension method was built
//    from the parameters' syntactic types only, which an import does not
//    have, so `k.Foo` over `extension K` was "not implicitly convertible"
//    to `(int) -> int` (§5.1).
//
// §27.6: "the resulting function has the receiver as its first parameter" for
// an unbound reference; a bound one captures the receiver. §2.8.2 clause 2:
// an external instantiation "behaves identically to a same-module" one.
//
// A free-variable head (`extension IList<T>`) is NOT exported by this fix and
// stays as it was (§26.4 block-level type parameters need #783 machinery).

#include <string>

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::ModuleSource;
using bezel::test::diagnosticMessageContains;

namespace {

const char* kLib =
    "module N2;\n"
    "public class K\n"
    "  public constructor() end constructor;\n"
    "end K;\n"
    "public extension K\n"
    "  public method Inc(a: int): int\n"
    "    return a + 1;\n"
    "  end Inc;\n"
    "end;\n"
    "public extension IList<int>\n"
    "  public method Foo(a: int): int\n"
    "    return a;\n"
    "  end Foo;\n"
    "end;\n";

JitResult run(const std::string& body) {
    std::string app =
        "module M;\n"
        "import System;\n"
        "import System.Collections.Linq;\n"
        "import N2;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var a: Array<int> := [1,2,3];\n"
        "    var il: IList<int> := a;\n" +
        body +
        "    return 0;\n"
        "  end Main;\n"
        "end;\n";
    return bezel::test::runAotModulesChecked({{"N2", kLib}, {"M", app}}, "M");
}

void expectOut(const std::string& body, const std::string& want) {
    JitResult r = run(body);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}

}  // namespace

// The issue's repro.
BEZEL_TEST(xmod_extension_method_ref_6898, bound_reference_to_ground_generic_extension_spec_27_6) {
    expectOut("    var g: (int) -> int := il.Foo;\n"
              "    Console.WriteLine(g(5));\n",
              "5\n");
}

BEZEL_TEST(xmod_extension_method_ref_6898, call_of_ground_generic_extension_spec_26_5) {
    expectOut("    Console.WriteLine(il.Foo(7));\n", "7\n");
}

BEZEL_TEST(xmod_extension_method_ref_6898, bound_reference_to_class_extension_spec_27_6) {
    expectOut("    var k := new K();\n"
              "    var g: (int) -> int := k.Inc;\n"
              "    Console.WriteLine(g(5));\n",
              "6\n");
}

BEZEL_TEST(xmod_extension_method_ref_6898, unbound_reference_to_class_extension_spec_27_6) {
    expectOut("    var k := new K();\n"
              "    var g: (K, int) -> int := K.Inc;\n"
              "    Console.WriteLine(g(k, 5));\n",
              "6\n");
}

// ---------------------------------------------------------------------------
// Ground generic heads now cross the boundary, so the importer must bind the
// body the same-module program binds (§2.8.2 clause 2), never another block's.
// These rows were found by the #6898 review (#6946): with symbols mangled by
// head name only, the importer bound `bz$ext$L.Box$Foo`, the FIRST-defined
// block's body (an internal `Box<long>` printed 100 for a `Box<int>`).

namespace {

const char* kBoxHead =
    "public class Box<T>\n"
    "  public v: T;\n"
    "  public constructor(a: T) v := a; end constructor;\n"
    "end Box;\n";

JitResult runLA(const std::string& lib, const std::string& app) {
    return bezel::test::runAotModulesChecked({{"L", lib}, {"A", app}}, "A");
}

const char* kBoxIntFooMain =
    "class P public static method Main(): int\n"
    "  var bi := new Box<int>(1);\n"
    "  Console.WriteLine($\"{bi.Foo()}\");\n"
    "  return 0; end Main; end P;\n";

}  // namespace

// #6946's repro: an internal `extension Box<long>` defined first, then the
// exported `public extension Box<int>`, both declaring `Foo`. Same module: 1.
BEZEL_TEST(xmod_extension_method_ref_6898, ground_block_after_internal_same_name_binds_own_body_spec_2_8_2) {
    std::string lib = std::string("module L;\nimport System;\n") + kBoxHead +
                      "extension Box<long>\n"
                      "  public method Foo(): int => 100;\n"
                      "end;\n"
                      "public extension Box<int>\n"
                      "  public method Foo(): int => 1;\n"
                      "end;\n";
    std::string app =
        std::string("module A;\nimport System;\nimport L;\n") + kBoxIntFooMain;
    JitResult r = runLA(lib, app);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n");
}

// `public extension Box<T>` and `public extension Box<int>`, both `Foo`: the
// same-module call is §26.5.1's ambiguity (identical substituted heads). The
// free-variable block is not exported (§26.4), so the importer must not see
// the ground block's `Foo` alone and bind it: it finds no `Foo` and rejects.
const char* kFreeAndGroundLib =
    "public extension Box<T>\n"
    "  public method Foo(): int => 5;\n"
    "end;\n"
    "public extension Box<int>\n"
    "  public method Foo(): int => 1;\n"
    "end;\n";

BEZEL_TEST(xmod_extension_method_ref_6898, free_and_ground_same_name_same_module_ambiguous_spec_26_5_1) {
    std::string prog = std::string("module A;\nimport System;\n") + kBoxHead +
                       kFreeAndGroundLib + kBoxIntFooMain;
    JitResult r = bezel::test::runAotModules({{"A", prog}}, "A");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "ambiguous extension member `Foo`"));
}

BEZEL_TEST(xmod_extension_method_ref_6898, free_and_ground_same_name_cross_module_not_bound_spec_26_5_1) {
    std::string lib = std::string("module L;\nimport System;\n") + kBoxHead +
                      kFreeAndGroundLib;
    std::string app =
        std::string("module A;\nimport System;\nimport L;\n") + kBoxIntFooMain;
    JitResult r = runLA(lib, app);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "no member `Foo`"));
}

// Every block exported: two ground heads per generic type, on a class
// (`Box<int>` / `Box<long>`) and on an interface (`IEnumerable<int>` /
// `IEnumerable<long>`), called and taken as bound references.
const char* kAllExportedLib =
    "module L;\n"
    "public class Box<T>\n"
    "  public v: T;\n"
    "  public constructor(a: T) v := a; end constructor;\n"
    "end Box;\n"
    "public extension Box<int>\n"
    "  public method Foo(): int => 1;\n"
    "end;\n"
    "public extension Box<long>\n"
    "  public method Foo(): int => 100;\n"
    "end;\n"
    "public extension Box<bool>\n"
    "  public method Bar(): string => \"bool\";\n"
    "end;\n"
    "public extension Box<string>\n"
    "  public method Bar(): string => \"str\";\n"
    "end;\n"
    "public extension IEnumerable<int>\n"
    "  public method Cnt(): int => 22;\n"
    "end;\n"
    "public extension IEnumerable<long>\n"
    "  public method Cnt(): int => 33;\n"
    "end;\n";

BEZEL_TEST(xmod_extension_method_ref_6898, all_exported_ground_blocks_calls_and_bound_refs_spec_27_6) {
    std::string app =
        "module A;\n"
        "import System;\n"
        "import System.Collections.Linq;\n"
        "import L;\n"
        "class P public static method Main(): int\n"
        "  var a: Array<long> := [1L];\n"
        "  var b: Array<int> := [1];\n"
        "  var bl := new Box<long>(1L);\n"
        "  var bi := new Box<int>(1);\n"
        "  var bs := new Box<string>(\"s\");\n"
        "  var bb := new Box<bool>(true);\n"
        "  Console.WriteLine($\"{a.Cnt()} {b.Cnt()} {bl.Foo()} {bi.Foo()} {bs.Bar()} {bb.Bar()}\");\n"
        "  var f := bl.Foo; var g := bi.Foo;\n"
        "  Console.WriteLine($\"{f()} {g()}\");\n"
        "  return 0; end Main; end P;\n";
    JitResult r = runLA(kAllExportedLib, app);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "33 22 100 1 str bool\n100 1\n");
}

// The importer adds its own ground blocks on the imported heads (#6946's
// duplicate-symbol half) and still reaches the library's.
BEZEL_TEST(xmod_extension_method_ref_6898, importer_ground_blocks_beside_imported_ones_spec_26_6) {
    std::string app =
        "module A;\n"
        "import System;\n"
        "import System.Collections.Linq;\n"
        "import L;\n"
        "public extension Array<string>\n"
        "  public method Cnt(): int => 44;\n"
        "end;\n"
        "public extension Box<short>\n"
        "  public method Foo(): int => 55;\n"
        "end;\n"
        "class P public static method Main(): int\n"
        "  var a: Array<long> := [1L];\n"
        "  var s: Array<string> := [\"x\"];\n"
        "  var bsh := new Box<short>(1);\n"
        "  var bl := new Box<long>(1L);\n"
        "  Console.WriteLine($\"{a.Cnt()} {s.Cnt()} {bsh.Foo()} {bl.Foo()}\");\n"
        "  return 0; end Main; end P;\n";
    JitResult r = runLA(kAllExportedLib, app);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "33 44 55 100\n");
}
