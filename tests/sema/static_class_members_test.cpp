// Spec-derived tests for Sema slice S16 — a static class may contain
// only static members (§11.7).

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
    }
    return r;
}

} // namespace

BEZEL_TEST(sema_static_class, static_method_in_static_class_ok) {
    auto r = parseSemaAll(
        "module M; "
        "static class Utils "
        "  public static method Max(a: int, b: int): int => a; "
        "end Utils;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_static_class, instance_method_in_static_class_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "static class Utils "
        "  public method Inst() _ := 1; end Inst; "
        "end Utils;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_static_class, instance_field_in_static_class_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "static class Utils "
        "  public _x: int := 0; "
        "end Utils;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_static_class, static_field_in_static_class_ok) {
    auto r = parseSemaAll(
        "module M; "
        "static class Utils "
        "  public static _count: int := 0; "
        "end Utils;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_static_class, destructor_in_static_class_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "static class Utils "
        "  public static method M() _ := 1; end M; "
        "  destructor end destructor; "
        "end Utils;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_static_class, instance_member_in_normal_class_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class Normal "
        "  public _x: int := 0; "
        "  public method Inst() _ := 1; end Inst; "
        "end Normal;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_static_class, static_const_in_static_class_ok) {
    auto r = parseSemaAll(
        "module M; "
        "static class Constants "
        "  public const Pi: double := 3.14; "
        "end Constants;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §11.7 (v0.181, issue #1557): lifting an unrecorded leftover of the v0.55
// ban, a static class MAY declare a `static owned readonly` field — the
// canonical resource-holding static (§11.5) — because static exit disposal
// (§22.13) is runtime-driven and independent of instance `Dispose()`, so the
// no-instantiation reasoning that forbids instance members never applied to
// it. Before this fix, this was rejected with "static class `Globals` may
// not declare an owned field `Held` (§11.7)".
BEZEL_TEST(sema_static_class, static_owned_readonly_field_ok_1557) {
    auto r = parseSemaAll(
        "module M; "
        "class Held end Held; "
        "static class Globals "
        "  public static owned readonly Cache: Held := new Held(); "
        "end Globals;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §11.7 / §11.5 (#1557): a mutable `static owned` field (no `readonly`) on a
// static class is accepted too, when its type clears the orthogonal §22.12
// rule 2 check (a mutable static field of class/interface type must be one
// of the compiler-known internally-synchronized types — Mutex, Semaphore,
// Atomic<T>, etc.). `Mutex` qualifies, so this isolates the #1557 rejection
// (now lifted) from that separate, unrelated rule: a user class like `Held`
// would still be rejected here by rule 2 alone, exactly as it would be on an
// ordinary (non-static) class — the lifted §11.7 ban does not carve out a
// narrower rule for static classes specifically.
BEZEL_TEST(sema_static_class, static_owned_mutable_field_ok_1557) {
    auto r = parseSemaAll(
        "module M; "
        "static class Globals "
        "  public static owned Lock: Mutex? := new Mutex(); "
        "end Globals;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §11.7 (#1557): a static owning-inline-array field (`[N owned]T`) on a
// static class is accepted. This combination never carried the field-level
// `owned` modifier the old rejection inspected (the array's `owned` marker
// lives in the TYPE, `[N owned]T` — §15.3), so it was never rejected even
// before this fix; this locks the acceptance in as a regression guard.
BEZEL_TEST(sema_static_class, static_owning_inline_array_field_ok_1557) {
    auto r = parseSemaAll(
        "module M; "
        "class Slot end Slot; "
        "static class Globals "
        "  public static _arr: [2 owned]Slot? := [new Slot(), new Slot()]; "
        "end Globals;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
