// Issue #7680 -- the stdlib enums the driver injects as prelude declarations
// (DayOfWeek / DateTimeKind of stdlib §2.5, SeekOrigin / FileMode / FileAccess
// of stdlib §7) render by the enum default ToString.
//
// Spec §8.5 (v0.350): "For body records that do not satisfy auto-equality
// conditions, and for enums: the type's qualified name (the stdlib §2.13
// `FullName` rendering, as for classes)." The prelude declarations are parsed
// INTO the user's module, so the rendering named the USER's module
// ("M.DayOfWeek"); the type is the stdlib's, declared in `System`
// (stdlib §2, "`System` (auto-imported)"), so its qualified name is
// "System.DayOfWeek" (the System.IO enums: "System.IO.FileMode"). A user enum
// keeps its own module ("M.Color"), and a user enum that shadows a prelude
// name (spec §2.3 shadowing) renders as the user's.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

std::string prog(const std::string& decls, const std::string& body) {
    return "module M; " + decls +
           " class C public static method Main(): int " + body +
           " return 0; end Main; end C;";
}

}  // namespace

// §8.5 / stdlib §2.5: direct print, local, interpolation, ToString().
BEZEL_TEST(prelude_enum_fullname_7680, spec_8_5_system_enums_render_system_qualified) {
    auto r = runAot(prog("",
        "var w: DayOfWeek := DayOfWeek.Monday; "
        "var k: DateTimeKind := DateTimeKind.Utc; "
        "Console.WriteLine(DateTimeKind.Local); "
        "Console.WriteLine(w); "
        "Console.WriteLine($\"{k}\"); "
        "Console.WriteLine(w.ToString()); "
        "Console.WriteLine(new DateTime(2020, 1, 2, 3, 4, 5, 0).DayOfWeek); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
        "System.DateTimeKind\nSystem.DayOfWeek\nSystem.DateTimeKind\n"
        "System.DayOfWeek\nSystem.DayOfWeek\n");
}

// §8.5 / stdlib §7: the System.IO prelude enums are in System.IO.
BEZEL_TEST(prelude_enum_fullname_7680, spec_8_5_system_io_enums_render_system_io_qualified) {
    auto r = runAot(prog("",
        "var m: FileMode := FileMode.Open; "
        "Console.WriteLine(m); "
        "Console.WriteLine($\"{FileAccess.Read}\"); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "System.IO.FileMode\nSystem.IO.FileAccess\n");
}

// §8.5 / §2.3: a user enum keeps its own module, including one that shadows a
// prelude name.
BEZEL_TEST(prelude_enum_fullname_7680, spec_8_5_user_enums_keep_their_module) {
    auto r = runAot(prog(
        "enum Color Red, Green; end Color; "
        "enum DayOfWeek Mon, Tue; end DayOfWeek; ",
        "Console.WriteLine(Color.Green); "
        "Console.WriteLine(DayOfWeek.Tue); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "M.Color\nM.DayOfWeek\n");
}
