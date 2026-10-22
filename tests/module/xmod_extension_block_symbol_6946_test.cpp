// Regression test for issue #6946 (§26.6 / §26.4 / §2.8.3 rule 3, v0.340).
//
// An extension method's symbol was mangled by the extended type's head name
// only (`bz$ext$L.Box$Foo`), so it named the TYPE, not the extension BLOCK:
//
//  * an importing module that declares its own extension block on an
//    imported type, with a member of the same name as one of the exporter's
//    blocks, emitted the exporter's very symbol, and the link failed with
//    `duplicate symbol: bz$ext$L.Box$Foo` (generic head, `extension Box<short>`
//    against L's `extension Box<long>`) or `bz$ext$L.K$Foo` (plain class head);
//  * two blocks on one generic head in one module (`extension Box<int>` and
//    `extension Box<long>`) were told apart only by LLVM's uniquifier
//    (`Foo` / `Foo.2`) in definition order, which an importer that sees only
//    the exported blocks cannot reproduce (the #6898 rows pin that half).
//
// The symbol now carries the head's argument spelling and, for an extension
// declared outside the target's module, the extension's own module. §26.6:
// blocks for the same type are merged, "Duplicate signatures are a compile
// error", so distinct signatures are distinct definitions; §2.8.3 rule 3: a
// module's declarations are distinct from another module's.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;

namespace {

void expectOut(const char* lib, const std::string& app,
               const std::string& want) {
    JitResult r =
        bezel::test::runAotModulesChecked({{"L", lib}, {"A", app}}, "A");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}

}  // namespace

// The issue's measured link failure: A's own `extension Box<short>` `Foo`
// against L's `extension Box<long>` `Foo`.
BEZEL_TEST(xmod_extension_block_symbol_6946, importer_block_on_imported_generic_head_links_spec_26_6) {
    const char* lib =
        "module L;\n"
        "import System;\n"
        "public class Box<T>\n"
        "  public v: T;\n"
        "  public constructor(a: T) v := a; end constructor;\n"
        "end Box;\n"
        "public extension Box<long>\n"
        "  public method Foo(): int => 100;\n"
        "end;\n";
    std::string app =
        "module A;\n"
        "import System;\n"
        "import L;\n"
        "public extension Box<short>\n"
        "  public method Foo(): int => 55;\n"
        "end;\n"
        "class P public static method Main(): int\n"
        "  var bsh := new Box<short>(1);\n"
        "  Console.WriteLine($\"{bsh.Foo()}\");\n"
        "  return 0; end Main; end P;\n";
    expectOut(lib, app, "55\n");
}

// The plain-class twin: A's `extension K` declares a `Foo` that L's
// `extension K` also declares (never called, which §26.5 rule 4 would reject
// as ambiguous); the definitions must still link.
BEZEL_TEST(xmod_extension_block_symbol_6946, importer_block_on_imported_class_head_links_spec_26_6) {
    const char* lib =
        "module L;\n"
        "import System;\n"
        "public class K\n"
        "  public v: int;\n"
        "  public constructor(a: int) v := a; end constructor;\n"
        "end K;\n"
        "public extension K\n"
        "  public method Foo(): int => 1;\n"
        "end;\n";
    std::string app =
        "module A;\n"
        "import System;\n"
        "import L;\n"
        "extension K\n"
        "  public method Foo(): int => 2;\n"
        "  public method Bar(): int => this.v + 10;\n"
        "end;\n"
        "class P public static method Main(): int\n"
        "  var k := new K(1);\n"
        "  Console.WriteLine($\"{k.Bar()}\");\n"
        "  return 0; end Main; end P;\n";
    expectOut(lib, app, "11\n");
}
