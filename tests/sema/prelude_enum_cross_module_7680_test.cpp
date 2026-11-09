// Regression test for issue #7680 (residual leg 2) — the auto-imported stdlib
// prelude enums are ONE type across modules.
//
// THE DEFECT. The driver injects the stdlib prelude declarations into EVERY
// compilation unit (`src/driver/Driver.cpp`'s `injectStdlibPrelude`), so every
// module parses its OWN copy of `DayOfWeek` / `DateTimeKind` (stdlib §2.5) —
// and each copy is declared in the USER's module, so under spec §2.8.3 rule 3
// the copies are DIFFERENT types:
//
//   §2.8.3 rule 3 *Program-wide type identity* (v0.355): "A type is identified
//   program-wide by its declaring module name, its qualified type name
//   (including any enclosing-type path, §11.6) ... and its generic arity ...
//   Two modules each declaring a public type of the same name declare two
//   unrelated types."
//
// The stdlib declares exactly ONE `DayOfWeek` (stdlib §2.5, the auto-imported
// `System`), so the copies must be one type. At a module boundary they were
// not: the exporter does not export the prelude decl (every importer injects
// its own copy), so the importer's reference to the dependency's `DayOfWeek`
// arrived UNBOUND — an opaque type, which the §8.4 enum-equality rule then
// rejected against the importing module's own `DayOfWeek`:
//
//   Main.bzl:5:23: operator `=` compares enum `DayOfWeek` with a non-enum
//   operand — an enum may be compared only with a value of the same enum
//   type (§8.4 / §10).
//
// and which the importer also lowered as the i32 placeholder — so the value
// read back from the exporter's BYTE-wide enum return slot was garbage.
//
// THE ORACLE. `runAotModules` is the strict AOT oracle for a MULTI-MODULE
// program (the real driver + clang link + native exec, fork-isolated). Every
// admitting row asserts the program's STDOUT — the program's BEHAVIOUR, not
// "it compiled". The refusing row asserts the exact §8.4 diagnostic, so the
// shadowing control cannot pass by a silent drop.
//
// ASan is OFF in lanes by default (CLAUDE.md): this is type identity, not
// allocation, layout, retain/release or disposal.

#include <iostream>
#include <string>
#include <vector>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::ModuleSource;
using bezel::test::runAotModules;

namespace {

// The compiler's own diagnostic is the most useful thing a failing row can
// carry, so a row that did not run prints it.
void reportDiagnostics(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        std::cerr << "[prelude_enum_cross_module_7680] " << d.message << "\n";
}

// The dependency: a library whose public surface carries a stdlib §2.5
// prelude enum. The ENUM is not exported as a type of its own (the driver
// injects a copy into every module), so the importer sees it only through
// this signature.
std::string libWith(const std::string& decl) {
    return "module Lib;\n" + decl +
           "public class L\n"
           "  public static method Mk(): DayOfWeek => DayOfWeek.Friday;\n"
           "  public static method Kind(): DateTimeKind => DateTimeKind.Utc;\n"
           "end L;\n";
}

// The importing module: `import Lib;` and a body under test. §21.7: a method
// with a declared return type must return on EVERY path, so the body is
// closed with the `return 0;` every row needs.
JitResult runApp(const std::string& libSrc, const std::string& body,
                 const std::string& appDecls = "") {
    std::string app =
        "module App;\n"
        "import Lib;\n" + appDecls +
        "public class Program\n"
        "  public static method Main(): int\n" + body +
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
    return runAotModules({ModuleSource{"Lib", libSrc}, ModuleSource{"App", app}},
                         "App");
}

// Does the rejection carry the §8.4 enum-equality diagnostic? The message
// names the enum and what it was compared with — "a non-enum operand" (the
// pre-fix shape: the imported reference was an opaque type) or "a different
// enum `X`" (two distinct enums) — so the needle stops at the "with".
bool diagIsEnumEquality(const JitResult& r, const char* op, const char* name) {
    const std::string needle = std::string("operator `") + op +
                               "` compares enum `" + name + "` with";
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// §2.8.3 rule 3: the imported `DayOfWeek` and the importing module's own
// `DayOfWeek` are ONE type, so §8.4's same-enum equality admits the
// comparison. Pre-fix: "compares enum `DayOfWeek` with a non-enum operand".
BEZEL_TEST(sema_prelude_enum_cross_module_7680,
           spec_2_8_3_r3_imported_prelude_enum_equality_is_admitted) {
    auto r = runApp(libWith(""),
        "    Console.WriteLine(Lib.L.Mk() = DayOfWeek.Friday);\n");
    if (!r.ran) reportDiagnostics(r);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n");
}

// The reversed operand order is the same comparison.
BEZEL_TEST(sema_prelude_enum_cross_module_7680,
           spec_2_8_3_r3_reversed_equality_is_admitted) {
    auto r = runApp(libWith(""),
        "    Console.WriteLine(DayOfWeek.Friday = Lib.L.Mk());\n");
    if (!r.ran) reportDiagnostics(r);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n");
}

// `<>` is the other half of §8.4's enum comparison.
BEZEL_TEST(sema_prelude_enum_cross_module_7680,
           spec_2_8_3_r3_imported_prelude_enum_inequality_is_admitted) {
    auto r = runApp(libWith(""),
        "    Console.WriteLine(Lib.L.Mk() <> DayOfWeek.Saturday);\n");
    if (!r.ran) reportDiagnostics(r);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n");
}

// The stdlib §2.5 twin: `DateTimeKind` is one type program-wide too.
BEZEL_TEST(sema_prelude_enum_cross_module_7680,
           spec_2_8_3_r3_imported_date_time_kind_equality_is_admitted) {
    auto r = runApp(libWith(""),
        "    Console.WriteLine(Lib.L.Kind() = DateTimeKind.Utc);\n");
    if (!r.ran) reportDiagnostics(r);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n");
}

// §8.5 / stdlib §2.13 rule 1: an enum's default ToString is its qualified
// type name, and the stdlib `DayOfWeek` is homed by `System` — so an IMPORTED
// value renders exactly as the same-module spelling does ("System.DayOfWeek",
// not the bare "DayOfWeek" and not the USER's module).
BEZEL_TEST(sema_prelude_enum_cross_module_7680,
           spec_8_5_imported_prelude_enum_renders_the_stdlib_qualified_name) {
    auto r = runApp(libWith(""),
        "    var d: DayOfWeek := Lib.L.Mk();\n"
        "    Console.WriteLine(d);\n");
    if (!r.ran) reportDiagnostics(r);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "System.DayOfWeek\n");
}

// stdlib §2.5: `enum DayOfWeek: byte` — the enum is BYTE-wide, so an imported
// value of it carries that width. Pre-fix the imported reference was opaque
// (lowered as the i32 placeholder), so the value read back from the
// exporter's byte-wide return slot was garbage ("1850181637" on the measured
// run).
BEZEL_TEST(sema_prelude_enum_cross_module_7680,
           spec_2_5_imported_prelude_enum_keeps_its_storage_width) {
    auto r = runApp(libWith(""),
        "    var d := Lib.L.Mk();\n"
        "    Console.WriteLine(d);\n");
    if (!r.ran) reportDiagnostics(r);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "System.DayOfWeek\n");
}

// §2.3 (shadowing) + §2.8.3 rule 3 (two same-named types are two types): a
// USER top-level `DayOfWeek` shadows the prelude, so the importing module has
// no stdlib `DayOfWeek` to bind to — the imported stdlib value and the user's
// own enum stay two types, and §8.4 rejects the comparison. This is the
// control that keeps the fix from re-typing a user's enum as the stdlib one.
BEZEL_TEST(sema_prelude_enum_cross_module_7680,
           spec_2_3_user_shadowed_prelude_enum_stays_a_different_type) {
    auto r = runApp(libWith(""),
        "    Console.WriteLine(Lib.L.Mk() = DayOfWeek.Tue);\n",
        "public enum DayOfWeek Mon, Tue; end DayOfWeek;\n");
    if (r.ran) reportDiagnostics(r);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "");
    BEZEL_EXPECT_TRUE(diagIsEnumEquality(r, "=", "DayOfWeek"));
}

// The control: a USER enum exported by the dependency is one type across the
// boundary already (§2.8.3 rule 3), and the fix must not change that.
BEZEL_TEST(sema_prelude_enum_cross_module_7680,
           spec_2_8_3_r3_user_enum_across_modules_is_unaffected) {
    std::string lib =
        "module Lib;\n"
        "public enum Color Red, Green; end Color;\n"
        "public class L\n"
        "  public static method Mk(): Color => Color.Green;\n"
        "end L;\n";
    std::string app =
        "module App;\n"
        "import Lib;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(Lib.L.Mk() = Lib.Color.Green);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
    auto r = runAotModules({ModuleSource{"Lib", lib}, ModuleSource{"App", app}},
                           "App");
    if (!r.ran) reportDiagnostics(r);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n");
}

// The OTHER control, and the one that must NOT be admitted: the SAME user
// enum name declared in BOTH modules is TWO types (§2.8.3 rule 3 — "two
// modules each declaring a public type of the same name declare two
// unrelated types"), so the comparison is refused, before and after the fix.
BEZEL_TEST(sema_prelude_enum_cross_module_7680,
           spec_2_8_3_r3_same_user_enum_name_in_two_modules_stays_two_types) {
    std::string lib =
        "module Lib;\n"
        "public enum Color Red, Green; end Color;\n"
        "public class L\n"
        "  public static method Mk(): Color => Color.Green;\n"
        "end L;\n";
    std::string app =
        "module App;\n"
        "import Lib;\n"
        "public enum Color Red, Green; end Color;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(Lib.L.Mk() = Color.Green);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
    auto r = runAotModules({ModuleSource{"Lib", lib}, ModuleSource{"App", app}},
                           "App");
    if (r.ran) reportDiagnostics(r);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "");
    BEZEL_EXPECT_TRUE(diagIsEnumEquality(r, "=", "Color"));
}

// stdlib §2.5's member-read shape (the #7536 line): the dependency reads
// `DateTime.DayOfWeek` and returns it, and the importer compares the
// returned prelude enum with its own `DayOfWeek`. The verifier's probe
// `p-xmod2` is this shape.
BEZEL_TEST(sema_prelude_enum_cross_module_7680,
           spec_2_5_imported_day_of_week_member_read_is_admitted) {
    std::string lib =
        "module Lib;\n"
        "public class L\n"
        "  public static method Dow(d: DateTime): DayOfWeek => d.DayOfWeek;\n"
        "end L;\n";
    std::string app =
        "module App;\n"
        "import Lib;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var d := new DateTime(2024, 3, 15);\n"
        "    Console.WriteLine(Lib.L.Dow(d) = DayOfWeek.Friday);\n"
        "    Console.WriteLine($\"{d.DayOfWeek}\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
    auto r = runAotModules({ModuleSource{"Lib", lib}, ModuleSource{"App", app}},
                           "App");
    if (!r.ran) reportDiagnostics(r);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nSystem.DayOfWeek\n");
}

// The stdlib §7.2 twin of the SAME mechanism: `FileMode` is a prelude enum
// of the always-available `System.IO` module, so it is one type program-wide
// on the same terms as `DayOfWeek`.
BEZEL_TEST(sema_prelude_enum_cross_module_7680,
           spec_7_2_imported_system_io_prelude_enum_equality_is_admitted) {
    std::string lib =
        "module Lib;\n"
        "public class L\n"
        "  public static method Mk(): FileMode => FileMode.Append;\n"
        "end L;\n";
    std::string app =
        "module App;\n"
        "import Lib;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(Lib.L.Mk() = FileMode.Append);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
    auto r = runAotModules({ModuleSource{"Lib", lib}, ModuleSource{"App", app}},
                           "App");
    if (!r.ran) reportDiagnostics(r);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n");
}
