// fix #9247 (§26.5 rule 2 / §26.4 / §2.8.2, v0.359) — an IMPORTED `extension`
// block over a BUILT-IN head is APPLIED in the importing module.
//
// THE DEFECT. A library module declares
//
//     module A;
//     import System.IO;
//     public extension TextReader
//       public method Bogus(): int => 7;
//     end;
//
// and an importing module `B` writes
//
//     var t: TextReader := Console.In;
//     var v := t.Bogus();
//
// §26.5 rule 2 (v0.359) is normative and decides this case: "If the receiver's
// type declares no instance member of that name, the compiler searches THE
// CURRENT MODULE AND ALL IMPORTED MODULES … for extension blocks on `x`'s type,
// base types, or implemented interfaces". §2.2 (v0.359) says the same of
// extension members: "they are candidates only from the current module and
// imported modules (§26.5 rule 2)". So the imported block IS a candidate and
// its member must be applied — exactly as the SAME block declared in the
// importing module is (MEASURED: the same-module program prints `7`).
//
// MEASURED ON THE BASE (r30-c1 @ 009b0c5de, `bezelc` rebuilt): the importer
// REFUSED the call with §2.8.2's shape diagnostic —
//
//     B2.bzl:7:14: no member `Bogus` on type `TextReader`: the extension block
//     `extension TextReader` in module `A` (declared at A.bzl:3:8) declares
//     `Bogus`, and §26.5.1 selects that block for this receiver — but the
//     artifact interface cannot carry that block's shape: ITS HEAD BINDS A
//     FREE TYPE VARIABLE, so the exported symbol was compiled against the
//     exporting module's placeholder layout and dispatching it here would
//     need importer-side monomorphization (§26.4). …
//
// THE ROOT CAUSE. The artifact interface classifies an extension block as
// RESOLUTION-ONLY ("shape-only") when its head "binds a free type variable",
// through the shared predicate `sema::typeRefBindsFreeTypeVariable`
// (src/sema/TypeRef.hpp), whose first clause is `declaration == nullptr &&
// genericArgs.empty()`. A COMPILER BUILT-IN has no `ast::TypeDecl`, so its
// TypeRef carries `declaration == nullptr` — EXACTLY the shape a free type
// variable has. So `extension TextReader` / `extension Random` /
// `extension DateTime` was classified as a free-variable head, the exporter
// shipped the block as a shape, and the importer refused to dispatch it —
// although the head names a CONCRETE built-in class and the exporter DID emit
// the block's `bz$ext$<module>.TextReader$<M>` symbol (MEASURED with `nm`).
//
// §26.4 (v0.359) says where a free type variable comes from, and it says it
// precisely: "IN THE EXTENDED TYPE'S ARGUMENT POSITIONS, each bare
// single-segment non-nullable identifier introduces a fresh block-level type
// parameter — always". The head's OWN name never introduces one, and §26.2
// rejects a head that is a type parameter outright ("Type parameters
// (`extension T` is meaningless)"). A head that names NO resolvable type at
// all is a different case, and it never reaches the artifact interface: the
// resolver reports "unresolved type name" and the declaration is refused
// (MEASURED). ⇒ the over-broad clause fired on exactly the built-in head.
//
// The fix narrows the shared predicate to the HEAD question (the argument
// positions), on BOTH sides — the exporter
// (module/ModuleMetadataBuilder.cpp) and the importer
// (sema/ExprTypecheck.cpp) — so the two cannot drift.
//
// WHY THERE ARE TWO ROWS. The first is the ISSUE'S OWN PROGRAM: the
// `TextReader` head, the `Console.In` receiver, the `Bogus()` member, and the
// `7` it must print. The second uses a built-in whose value is READ INSIDE
// the extension body (`this.Year * 2` over `DateTime`), so the row pins that
// the RECEIVER's VALUE crosses the module boundary intact and is read there —
// §2.8.2 clause 2 ("an external instantiation behaves identically to a
// same-module instantiation"). The first row's receiver is `Console.In`, which
// this compiler has no runtime representation for yet (a separate lane), so
// only the second row can pin the value transfer.
//
// FAILS WITHOUT THE FIX: both importers are REFUSED at compile time (§2.8.2's
// shape diagnostic above), so the program never runs and never prints — the
// rows fail on `r.ran` / `exitCode` / `stdoutText`. The same-module program
// (the control each row runs first) prints the same value on BOTH arms.
//
// The oracle is `runAotModules` — the strict AOT path (fork-isolated
// Driver::compileFile + clang link + native exec), never the deprecated
// in-process JIT harness.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::ModuleSource;

namespace {

// A cross-module program: `lib` is compiled as a library artifact, `app` is
// the importer.
JitResult xmod(const std::string& lib, const std::string& app) {
    return bezel::test::runAotModules(
        {ModuleSource{"A", "module A;\nimport System;\nimport System.IO;\n" +
                              lib},
         ModuleSource{"B", "module B;\nimport System;\nimport System.IO;\n"
                           "import A;\n" +
                               app}},
        "B");
}

// The SAME-MODULE control: one module holding both the library's declarations
// and the call. It is the answer the importer must agree with (§2.8.2 clause
// 2: an external instantiation "behaves identically to a same-module
// instantiation").
JitResult same(const std::string& lib, const std::string& app) {
    return bezel::test::runAotModules(
        {ModuleSource{"B", "module B;\nimport System;\nimport System.IO;\n" +
                               lib + "\n" + app}},
        "B");
}

}  // namespace

// ---------------------------------------------------------------------------
// The issue's own program: an imported `extension TextReader` block, a
// `TextReader` receiver, and the member it must apply.
// ---------------------------------------------------------------------------
BEZEL_TEST(imported_extension_builtin_head_9247,
           imported_textreader_extension_applies_26_5_rule_2) {
    // The issue's `A.bzl` verbatim (§26.5 rule 2 searches "the current module
    // and all imported modules").
    const std::string lib =
        "public extension TextReader\n"
        "  public method Bogus(): int => 7;\n"
        "end;\n";
    // The issue's `B2.bzl` verbatim (the TYPED-LOCAL receiver — the spelling
    // that works for a locally declared block).
    const std::string app =
        "public class Program\n"
        "  public static method Main()\n"
        "    var t: TextReader := Console.In;\n"
        "    var v := t.Bogus();\n"
        "    Console.WriteLine(v.ToString());\n"
        "  end Main;\n"
        "end Program;\n";

    // The control: the SAME block declared in the SAME module prints 7. It is
    // green on both arms — it pins that the shape is legal at all, and it is
    // the answer the importer must reproduce.
    JitResult s = same(lib, app);
    BEZEL_EXPECT_TRUE(s.ran);
    BEZEL_EXPECT_EQ(s.exitCode, 0);
    BEZEL_EXPECT_EQ(s.stdoutText, "7\n");

    // The importer: the member must be FOUND and DISPATCHED.
    //
    // FAILS WITHOUT THE FIX: the importer is refused with §2.8.2's shape
    // diagnostic ("its head binds a free type variable"), so the program never
    // runs and prints nothing.
    JitResult r = xmod(lib, app);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}

// ---------------------------------------------------------------------------
// The same defect with a receiver whose VALUE is read inside the extension
// body: the value must cross the module boundary and be read there (§2.8.2
// clause 2).
// ---------------------------------------------------------------------------
BEZEL_TEST(imported_extension_builtin_head_9247,
           imported_datetime_extension_reads_receiver_value_26_5_rule_2) {
    const std::string lib =
        "public extension DateTime\n"
        "  public method Year2(): int => this.Year * 2;\n"
        "end;\n";
    const std::string app =
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var d := new DateTime(2024, 3, 16);\n"
        "    Console.WriteLine(d.Year2().ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";

    // The control: `new DateTime(2024, 3, 16)` has the year 2024, so the
    // member reads 2024 and answers 4048.
    JitResult s = same(lib, app);
    BEZEL_EXPECT_TRUE(s.ran);
    BEZEL_EXPECT_EQ(s.exitCode, 0);
    BEZEL_EXPECT_EQ(s.stdoutText, "4048\n");

    // The importer: the receiver's VALUE must arrive intact (a wrong ABI
    // would not even link — the exporter shapes `this` by value for a value
    // built-in — and the read would answer a different year).
    //
    // FAILS WITHOUT THE FIX: the importer is refused with §2.8.2's shape
    // diagnostic, so the program never runs and prints nothing.
    JitResult r = xmod(lib, app);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4048\n");
}

// ---------------------------------------------------------------------------
// CONTROL (green on the base too): the extension reaches only the modules that
// IMPORT its declaring module. §2.8.3 rule 2 (v0.359): extension members "are
// candidates only from the current module and imported modules (§26.5 rule 2)
// … they are not reachable on a value whose module is unimported". Module `C`
// imports `B` (which imports `A`) but not `A`, so `Bogus` must NOT apply in
// `C` — the fix widens what an IMPORTER sees, never what a non-importer sees.
// ---------------------------------------------------------------------------
BEZEL_TEST(imported_extension_builtin_head_9247,
           extension_does_not_leak_into_a_module_that_does_not_import_it) {
    JitResult r = bezel::test::runAotModules(
        {ModuleSource{"A",
                      "module A;\nimport System;\nimport System.IO;\n"
                      "public extension TextReader\n"
                      "  public method Bogus(): int => 7;\n"
                      "end;\n"},
         ModuleSource{"B",
                      "module B;\nimport System;\nimport A;\n"
                      "public class Helper\n"
                      "  public static method Five(): int => 5;\n"
                      "end Helper;\n"},
         ModuleSource{"C",
                      "module C;\nimport System;\nimport System.IO;\nimport B;\n"
                      "public class Program\n"
                      "  public static method Main()\n"
                      "    var t: TextReader := Console.In;\n"
                      "    var v := t.Bogus();\n"
                      "    Console.WriteLine(v.ToString());\n"
                      "  end Main;\n"
                      "end Program;\n"}},
        "C");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`Bogus` on built-in type `TextReader`"));
}
