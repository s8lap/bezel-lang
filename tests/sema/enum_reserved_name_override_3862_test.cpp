// narrow #3862 (§10.4 "Methods, properties, static members", v0.293): every
// enum `E` carries three compiler-provided static members —
// `FromUnderlyingOrNull`, `FromNameOrNull`, `NameOf` — and "no user
// declaration, a member or a declared constant, may override or overload any
// of them". `Resolver::checkEnumRestrictions` (src/sema/TypeResolution_Checks.cpp)
// already rejected a user `FromUnderlyingOrNull` declaration (fix #3044), but
// had no arm at all for the OTHER two names in the same sentence.
//
// MEASURED on this HEAD (pre-fix, commit prior to this test): a program
// declaring `public static method FromNameOrNull(name: string): Priority? =>
// Low;` inside `enum Priority` compiled with `bezelc -c` at exit 0, zero
// diagnostics — the compiler-provided member's "total, never raises" contract
// was silently shadowed rather than protected. Same for a user `NameOf`
// declaration. This is exactly the corpus row
// `tests/conformance/rules/cases/s10_4_0005.bzl` ("a user declaration cannot
// overload the compiler-provided FromNameOrNull") — banked `fail` before this
// fix (the rule never fired: `expect: reject` scores `fail` iff the compiler
// exits 0).
//
// This fix adds `FromNameOrNull`/`NameOf` arms beside the existing
// `FromUnderlyingOrNull` one, rejecting a user member OR constant of either
// name at every nesting depth (the same `forEachTypeAndNested` walk fix #3601
// already routes the whole check through).
//
// Scope note: this fix is declaration-site rejection ONLY. It does not
// implement `FromNameOrNull`/`NameOf` as callable members (that needs a
// typecheck arm in ExprTypecheck_Calls.cpp and a codegen lowering in
// src/codegen/ — tracked by the parent issue #3862, which this fix narrows).

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/TypeResolution.hpp"

#include <string>

namespace {

bezel::test::SemaResult resolveSrc(const std::string& src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
    }
    return r;
}

int countDiags(const bezel::test::SemaResult& r, const char* needle) {
    int n = 0;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find(needle) != std::string::npos) ++n;
    }
    return n;
}

} // namespace

// The corpus's own repro (s10_4_0005): a user STATIC METHOD named
// `FromNameOrNull` on an enum is rejected, naming it as the
// compiler-provided member (§10.4).
BEZEL_TEST(sema_enum_reserved_name_override_3862,
           user_method_named_from_name_or_null_rejected) {
    auto r = resolveSrc(
        "module M; "
        "public enum Priority "
        "  Low, Medium, High; "
        "  public static method FromNameOrNull(name: string): Priority? "
        "    return Low; "
        "  end FromNameOrNull; "
        "end Priority; ");
    BEZEL_EXPECT_EQ(
        countDiags(r, "may not declare a member named `FromNameOrNull`"), 1);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// The sibling member, `NameOf` — same sentence, same rule (§10.4): "no user
// declaration ... may override or overload any of them."
BEZEL_TEST(sema_enum_reserved_name_override_3862,
           user_method_named_name_of_rejected) {
    auto r = resolveSrc(
        "module M; "
        "public enum Priority "
        "  Low, Medium, High; "
        "  public static method NameOf(value: Priority): string "
        "    return \"x\"; "
        "  end NameOf; "
        "end Priority; ");
    BEZEL_EXPECT_EQ(countDiags(r, "may not declare a member named `NameOf`"),
                    1);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §10.4 says "a member OR A DECLARED CONSTANT" — a constant literally named
// `NameOf` is rejected the same way a method is (the check runs on the
// Symbol's name before any member/constant dynamic_cast, the same reach the
// `FromUnderlyingOrNull` sibling arm already has, fix #3601).
BEZEL_TEST(sema_enum_reserved_name_override_3862,
           declared_constant_named_name_of_rejected) {
    auto r = resolveSrc(
        "module M; "
        "public enum Priority "
        "  Low, NameOf, High; "
        "end Priority; ");
    BEZEL_EXPECT_EQ(countDiags(r, "may not declare a member named `NameOf`"),
                    1);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// False-reject guard: a legal enum declaring none of the three reserved
// names must draw no diagnostic from this rule.
BEZEL_TEST(sema_enum_reserved_name_override_3862,
           legal_enum_not_newly_rejected) {
    auto r = resolveSrc(
        "module M; "
        "public enum Priority "
        "  Low, Medium, High; "
        "  public method IsUrgent(): bool => this = High; "
        "end Priority; ");
    BEZEL_EXPECT_EQ(countDiags(r, "compiler-provided"), 0);
}
