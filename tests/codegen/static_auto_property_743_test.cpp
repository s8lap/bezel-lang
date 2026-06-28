// Spec-derived tests for fix #743 — a STATIC auto-property gets a module-level
// global backing store, so the implicit get reads and the implicit set writes
// real storage.
//
// Authority: §23.1 — the compiler generates a backing field for each
// auto-property; for a static auto-property that backing is a module-level
// global. Pre-fix no global was emitted: the getter always returned 0 and the
// setter's store was discarded (the program below returned 0 instead of 5).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §23.1: the headline — set then read a static auto-property.
BEZEL_TEST(static_auto_property_743, set_then_get) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static property X: int; "
        "  public static method Main(): int "
        "    C.X := 5; "
        "    return C.X; "
        "  end Main; "
        "end C;"), 5);
}

// §23.1: a static auto-property reads as its zero value before any write.
BEZEL_TEST(static_auto_property_743, initial_value_is_zero) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static property X: int; "
        "  public static method Main(): int "
        "    return C.X; "  // never written → 0
        "  end Main; "
        "end C;"), 0);
}

// §23.1: writes persist and compose (read-modify-write).
BEZEL_TEST(static_auto_property_743, read_modify_write) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static property X: int; "
        "  public static method Main(): int "
        "    C.X := 5; "
        "    C.X := C.X + 10; "
        "    return C.X; "  // 15
        "  end Main; "
        "end C;"), 15);
}

// §23.1: the backing global is shared across methods — bare access inside the
// declaring class reaches the same storage.
BEZEL_TEST(static_auto_property_743, bare_access_across_methods) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static property Counter: int; "
        "  public static method Bump() Counter := Counter + 1; end Bump; "
        "  public static method Main(): int "
        "    Bump(); Bump(); Bump(); "
        "    return Counter; "  // 3
        "  end Main; "
        "end C;"), 3);
}

// §23.1: a reference-typed (string) static auto-property round-trips.
BEZEL_TEST(static_auto_property_743, string_static_auto_property) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static property Name: string; "
        "  public static method Main(): int "
        "    C.Name := \"hi\"; "
        "    return (1 if C.Name = \"hi\" else 0); "
        "  end Main; "
        "end C;"), 1);
}

// ── #1146 / §23.1: a static auto-property's CONSTANT inline initializer ───────
// (`static readonly property Total: int := 42`) is honored — the backing global
// previously read 0 (the initializer was dropped, always getNullValue).
BEZEL_TEST(static_auto_property_743, static_auto_property_int_initializer_1146) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static readonly property Total: int := 42; "
        "  public static method Main(): int return C.Total; end Main; "
        "end C;"), 42);
}

// A bool static auto-property initializer.
BEZEL_TEST(static_auto_property_743, static_auto_property_bool_initializer_1146) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static readonly property Flag: bool := true; "
        "  public static method Main(): int return (7 if C.Flag else 0); end Main; "
        "end C;"), 7);
}

// A string static auto-property initializer.
BEZEL_TEST(static_auto_property_743, static_auto_property_string_initializer_1146) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static readonly property Name: string := \"hello\"; "
        "  public static method Main(): int return C.Name.Length; end Main; "
        "end C;"), 5);
}
