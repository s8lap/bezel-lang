// Spec-derived sema tests for §22.12 statics-and-domains rules 2, 3, and 4
// (v0.163, issue #1363). Authority: §22.12 "Statics and domains" rules 1–5
// (v0.163).
//
// Rule 2: a mutable (non-readonly, non-const) static field or static
//   auto-property whose type is a user-declared class or interface is a
//   compile error unless the type is in the compiler-known internally-
//   synchronized set (Mutex, Semaphore, Atomic<T>, AtomicInteger<T>,
//   Channel<T>, ChannelSender<T>).  Any other class/interface type is
//   rejected at the declaration site.  [Primitive value types are not yet
//   checked: #1363-followup.]
//
// Rule 3 (pin): static readonly and const of immutable content (value types,
//   string, enums) are unrestricted — no errors or warnings.
//
// Rule 4: a static readonly (including owned-readonly) field of user-declared
//   class or interface type is admitted with a declaration-site warning naming
//   §22.12.  The warning is suppressed when the type is in the synchronized
//   set.
//
// Rule 1 and Rule 5 are runtime rules (see below for residue notes):
//   Rule 1 residue: the ensureinit guard is non-atomic (bz$T$initstate plain
//     load/store); concurrent-domain blocking unimplemented.  #1363-followup.
//   Rule 5 residue: bz_event_source_t carries no mutex; static event
//     subscribe/raise are not synchronized.  #1363-followup.

#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSema(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff any diagnostic references §22.12 rule 2 (mutable static class field).
bool hasMutableStaticError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("22.12") != std::string::npos &&
            d.message.find("mutable static") != std::string::npos)
            return true;
    return false;
}

// True iff any diagnostic is a §22.12 rule 4 warning (static readonly class).
bool hasStaticReadonlyWarning(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Warning &&
            d.message.find("22.12") != std::string::npos &&
            d.message.find("static readonly") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// §22.12 Rule 2: mutable static field of user-declared class type → error
// ---------------------------------------------------------------------------

// (1) §22.12 rule 2: a mutable static field of a user class type is an error.
// Probe: prior to fix, this was accepted silently.
BEZEL_TEST(sema_static_domains_1363,
           rule2_mutable_static_class_field_diagnoses) {
    auto r = parseSema(
        "module M; "
        "class Cfg public X: int := 0; end Cfg; "
        "class App "
        "  public static _cfg: Cfg := new Cfg(); "   // Rule 2 violation
        "  public static method Main(): int => 0; "
        "end App;");
    BEZEL_EXPECT_TRUE(hasMutableStaticError(r));
}

// (2) §22.12 rule 2: mutable static field of nullable class type is also an
// error — a nullable class reference is still a shared reference type.
BEZEL_TEST(sema_static_domains_1363,
           rule2_mutable_static_nullable_class_diagnoses) {
    auto r = parseSema(
        "module M; "
        "class Cfg public X: int := 0; end Cfg; "
        "class App "
        "  public static _cfg: Cfg?; "   // Rule 2 violation — nullable class
        "  public static method Main(): int => 0; "
        "end App;");
    BEZEL_EXPECT_TRUE(hasMutableStaticError(r));
}

// (3) §22.12 rule 2: mutable static field of interface type is an error.
BEZEL_TEST(sema_static_domains_1363,
           rule2_mutable_static_interface_field_diagnoses) {
    auto r = parseSema(
        "module M; "
        "interface IFoo method Get(): int; end IFoo; "
        "class App "
        "  public static _foo: IFoo?; "   // Rule 2 violation — interface
        "  public static method Main(): int => 0; "
        "end App;");
    BEZEL_EXPECT_TRUE(hasMutableStaticError(r));
}

// (4) §22.12 rule 2 positive: Mutex is in the synchronized set — admitted.
BEZEL_TEST(sema_static_domains_1363,
           rule2_mutable_static_mutex_admitted) {
    auto r = parseSema(
        "module M; "
        "class App "
        "  public static _lk: Mutex := new Mutex(); "
        "  public static method Main(): int => 0; "
        "end App;");
    BEZEL_EXPECT_FALSE(hasMutableStaticError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (5) §22.12 rule 2 positive: Semaphore is in the synchronized set — admitted.
BEZEL_TEST(sema_static_domains_1363,
           rule2_mutable_static_semaphore_admitted) {
    auto r = parseSema(
        "module M; "
        "class App "
        "  public static _sem: Semaphore := new Semaphore(1); "
        "  public static method Main(): int => 0; "
        "end App;");
    BEZEL_EXPECT_FALSE(hasMutableStaticError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (6) §22.12 rule 2 positive: Atomic<int> is in the synchronized set — admitted.
BEZEL_TEST(sema_static_domains_1363,
           rule2_mutable_static_atomic_admitted) {
    auto r = parseSema(
        "module M; "
        "class App "
        "  public static _counter: Atomic<int> := new Atomic<int>(0); "
        "  public static method Main(): int => 0; "
        "end App;");
    BEZEL_EXPECT_FALSE(hasMutableStaticError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (7) §22.12 rule 2 positive: Channel<int> is in the synchronized set — admitted.
BEZEL_TEST(sema_static_domains_1363,
           rule2_mutable_static_channel_admitted) {
    auto r = parseSema(
        "module M; "
        "class App "
        "  public static _ch: Channel<int> := new Channel<int>(0); "
        "  public static method Main(): int => 0; "
        "end App;");
    BEZEL_EXPECT_FALSE(hasMutableStaticError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// §22.12 Rule 3 (pin): static readonly / const of value types are unrestricted
// ---------------------------------------------------------------------------

// (8) §22.12 rule 3 pin: static const of primitive — no error.
BEZEL_TEST(sema_static_domains_1363,
           rule3_pin_static_const_primitive_admitted) {
    auto r = parseSema(
        "module M; "
        "class C "
        "  public static const MaxSize: int := 100; "
        "  public static method Main(): int => MaxSize; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (9) §22.12 rule 3 pin: static readonly of int — no error.
BEZEL_TEST(sema_static_domains_1363,
           rule3_pin_static_readonly_int_admitted) {
    auto r = parseSema(
        "module M; "
        "class C "
        "  public static readonly Count: int := 42; "
        "  public static method Main(): int => Count; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (10) §22.12 rule 3 pin: static readonly of string — no error.
BEZEL_TEST(sema_static_domains_1363,
           rule3_pin_static_readonly_string_admitted) {
    auto r = parseSema(
        "module M; "
        "class C "
        "  public static readonly Name: string := \"bezel\"; "
        "  public static method Main(): int => 0; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (11) §22.12 rule 3 pin: static readonly of an enum — no error.
BEZEL_TEST(sema_static_domains_1363,
           rule3_pin_static_readonly_enum_admitted) {
    auto r = parseSema(
        "module M; "
        "enum Color Red, Green, Blue; end Color; "
        "class C "
        "  public static readonly Default: Color := Color.Red; "
        "  public static method Main(): int => 0; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (12) §22.12 rule 3 pin: mutable static int — no error (primitive types are
//   not checked by the current Rule 2 implementation; see #1363-followup).
BEZEL_TEST(sema_static_domains_1363,
           rule3_pin_mutable_static_int_not_checked) {
    auto r = parseSema(
        "module M; "
        "class C "
        "  public static _count: int := 0; "
        "  public static method Main(): int => _count; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasMutableStaticError(r));
}

// ---------------------------------------------------------------------------
// §22.12 Rule 4: static readonly of user class type → declaration-site warning
// ---------------------------------------------------------------------------

// (13) §22.12 rule 4: a static readonly field of a user class type emits a
//   warning.  The class is reachable from every domain; the programmer must
//   confine its use to one domain or ensure internal synchronization.
BEZEL_TEST(sema_static_domains_1363,
           rule4_static_readonly_class_warns) {
    auto r = parseSema(
        "module M; "
        "class Cfg public X: int := 0; end Cfg; "
        "class App "
        "  public static readonly _cfg: Cfg := new Cfg(); "
        "  public static method Main(): int => 0; "
        "end App;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasStaticReadonlyWarning(r));
}

// (14) §22.12 rule 4: static readonly of nullable class type also warns.
BEZEL_TEST(sema_static_domains_1363,
           rule4_static_readonly_nullable_class_warns) {
    auto r = parseSema(
        "module M; "
        "class Cfg public X: int := 0; end Cfg; "
        "class App "
        "  public static readonly _cfg: Cfg?; "
        "  public static method Main(): int => 0; "
        "end App;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasStaticReadonlyWarning(r));
}

// (15) §22.12 rule 4 positive: static readonly Mutex — warning suppressed
//   (Mutex is in the synchronized set).
BEZEL_TEST(sema_static_domains_1363,
           rule4_static_readonly_mutex_no_warning) {
    auto r = parseSema(
        "module M; "
        "class App "
        "  public static readonly _lk: Mutex := new Mutex(); "
        "  public static method Main(): int => 0; "
        "end App;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasStaticReadonlyWarning(r));
}

// (16) §22.12 rule 4 positive: static readonly int — no warning (value type,
//   Rule 3 applies).
BEZEL_TEST(sema_static_domains_1363,
           rule4_static_readonly_int_no_warning) {
    auto r = parseSema(
        "module M; "
        "class C "
        "  public static readonly X: int := 5; "
        "  public static method Main(): int => X; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasStaticReadonlyWarning(r));
}

// (17) §22.12 rule 2: a mutable static auto-property of user class type is
//   an error.  Auto-properties with Auto bodyKind are storage-backed — they
//   are mutable shared state.
BEZEL_TEST(sema_static_domains_1363,
           rule2_mutable_static_autoprop_class_diagnoses) {
    auto r = parseSema(
        "module M; "
        "class Registry public X: int := 0; end Registry; "
        "class App "
        "  public static property Instance: Registry?; "
        "  public static method Main(): int => 0; "
        "end App;");
    BEZEL_EXPECT_TRUE(hasMutableStaticError(r));
}

// (18) §22.12 rule 4: a static readonly auto-property of user class type warns.
BEZEL_TEST(sema_static_domains_1363,
           rule4_static_readonly_autoprop_class_warns) {
    auto r = parseSema(
        "module M; "
        "class Registry public X: int := 0; end Registry; "
        "class App "
        "  public static readonly property Instance: Registry?; "
        "  public static method Main(): int => 0; "
        "end App;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasStaticReadonlyWarning(r));
}

// (19) §22.12 rule 2 positive: a static readonly auto-property of user class
//   type is NOT rule 2 (it's readonly, so rule 4 applies instead).
BEZEL_TEST(sema_static_domains_1363,
           rule2_readonly_autoprop_not_mutable_error) {
    auto r = parseSema(
        "module M; "
        "class Registry public X: int := 0; end Registry; "
        "class App "
        "  public static readonly property Instance: Registry?; "
        "  public static method Main(): int => 0; "
        "end App;");
    BEZEL_EXPECT_FALSE(hasMutableStaticError(r));
}
