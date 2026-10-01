// Spec-derived tests for #747 R42 #0 + the §22.12 ruling — accessing a STATIC
// member through an INSTANCE receiver (`c._x` where `_x` is `public static`).
//
// History: this originally SIGSEGV'd the compiler (fieldAddr dereferenced an
// end iterator). The crash was fixed (fieldAddr returns nullptr for a member not
// in the instance layout, routing to staticFieldGlobal). The owner then ruled
// (cf. C# CS0176) that an instance-receiver static access should be a SEMA
// diagnostic — static members are accessed through the type name only (§22.12).
//
// So the access is now rejected at sema (before codegen), which both enforces
// §22.12 and makes the original crash path unreachable. These tests assert the
// rejection; a type-qualified `C._x` access is unaffected.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "driver/Driver.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"

namespace {
namespace fs = std::filesystem;

// True when `source` FAILS to compile (a diagnostic is raised — NOT a crash).
bool compileFails(const std::string& source) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bezel_747sfi");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    { std::ofstream out(src); out << source; }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    std::remove(src.c_str());
    std::remove(exe.c_str());
    return !r.success;
}
}  // namespace

// §22.12: reading a static field through an instance receiver is rejected.
BEZEL_TEST(static_field_via_instance_747, read_through_instance_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; class C "
        "  public static _x: int := 7; "
        "  public static method Main(): int "
        "    using var c := new C(); return c._x; "
        "  end Main; "
        "end C;"));
}

// §22.12: writing a static field through an instance receiver is rejected.
BEZEL_TEST(static_field_via_instance_747, write_through_instance_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; class C "
        "  public static _x: int := 7; "
        "  public static method Main(): int "
        "    using var c := new C(); c._x := 9; return 0; "
        "  end Main; "
        "end C;"));
}

// §22.12: calling a static method through an instance receiver is rejected.
BEZEL_TEST(static_field_via_instance_747, static_method_through_instance_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; class C "
        "  public static method Get(): int => 9; "
        "  public static method Main(): int "
        "    using var c := new C(); return c.Get(); "
        "  end Main; "
        "end C;"));
}

// §22.12: a type-qualified static access `C._x` is fine — must compile.
BEZEL_TEST(static_field_via_instance_747, type_qualified_ok) {
    BEZEL_EXPECT_FALSE(compileFails(
        "module M; class C "
        "  public static _x: int := 7; "
        "  public static method Main(): int "
        "    C._x := 9; return C._x; "  // type-name receiver, read + write
        "  end Main; "
        "end C;"));
}

// §22.12: an ordinary instance member through an instance receiver is fine.
BEZEL_TEST(static_field_via_instance_747, instance_member_ok) {
    BEZEL_EXPECT_FALSE(compileFails(
        "module M; class C "
        "  public n: int := 5; "
        "  public static method Main(): int "
        "    using var c := new C(); return c.n; "
        "  end Main; "
        "end C;"));
}
