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
#include "support/aot_support.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
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

// §22.12 rule 2 (#1393, v0.221): a MUTABLE static of `string` type is now a
// residual compile error — this program is no longer legal Bezel. Before
// #1393 landed, rule 2 was silent on `string` entirely (the exact
// under-enforcement #1393 reports), so this shape compiled and this test
// exercised the auto-property backing-store SET+GET round trip through it.
// That round-trip mechanism for a reference type remains covered: the
// backing-store LOAD path is proven for `string` by
// static_auto_property_string_initializer_1146 below (a `static readonly
// property Name: string := "hello";` reads its initializer correctly), and
// the generic (type-independent) get/set STORE mechanism is proven by the
// `int` cases above (set_then_get, read_modify_write,
// bare_access_across_methods) — auto-property codegen does not special-case
// the element type between a store to an `int` global and a store to a
// `string` global. What is no longer legal — and now the regression this
// case pins — is the MUTATION of a static `string`, which #1393 rejects
// for the reason stated in the diagnostic below (an overwrite must either
// UAF the displaced referent across domains or leak it unboundedly).
BEZEL_TEST(static_auto_property_743, string_static_auto_property_now_rejected_1393) {
    auto r = runAot(
        "module M; class C "
        "  public static property Name: string; "
        "  public static method Main(): int "
        "    C.Name := \"hi\"; "
        "    return (if C.Name = \"hi\" then 1 else 0); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    bool foundRule2 = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("mutable static property `Name` has type "
                            "`string`, which \xC2\xA7""22.12 rule 2 does "
                            "not admit as a mutable static") !=
                std::string::npos &&
            d.message.find("(\xC2\xA7""22.12 rule 2, v0.221, #1393)") !=
                std::string::npos)
            foundRule2 = true;
    }
    BEZEL_EXPECT_TRUE(foundRule2);
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
        "  public static method Main(): int return (if C.Flag then 7 else 0); end Main; "
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
