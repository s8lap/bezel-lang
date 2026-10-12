// fix #5791 (stdlib §2.13, v0.334): a member access on a `Type`-typed value
// was SILENTLY ACCEPTED for every name — the program compiled at exit 0 with
// no diagnostic, and the enclosing statement degraded (the #2434
// "initializer expression failed to lower" zero-initialization).
//
// WHY A `Type`-TYPED PARAMETER, NOT `typeof()`/`GetType()`.
// A `Type` annotation resolves through `resolveNamed`'s `builtinTypeArity` arm
// to a `Kind::Named` TypeRef whose `declaration` is NULL (TypeResolution_Names
// .cpp) — NOT to `builtinClassSym("Type")`, which only ever stamps the RESULT
// of `GetType()`/`typeof`. Both of those are rejected upstream (#487 "reflection
// is not yet implemented", #511), so a row built on them never reaches member
// lookup and passes vacuously — the trap the first attempt at this fix fell
// into. A `Type`-typed PARAMETER is the honest route: it is admissible, it
// reaches `resolveMemberAccess`, and it is the receiver shape the defect
// actually lives on.
//
// These are pure-sema rows (`parseAndCheckOwnership`), not `runAot`: the
// lattice members of §2.13 have no codegen arm yet (§2.13's own
// "Spec-ahead-of-implementation" note, #3819), so an AOT row would pin a
// codegen gap that is a SEPARATE defect rather than the §2.13 surface itself.
// Every row asserts the SPECIFIC "no member" text, never merely
// `hasErrors()` — a row that passes on any unrelated earlier rejection is the
// defect this fix exists to remove.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// The §2.13 closed-surface diagnostic this fix introduces. Matching on both
// the member name and the surface citation means a row fails for the RIGHT
// reason, and cannot be satisfied by a different "no member" wording (e.g. the
// bare-type-parameter one, or §11.3's user-type one).
bool isTypeSurfaceReject(const OwnershipResult& r, const char* member) {
    const std::string name(member);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("no member `" + name + "` on type `Type`") !=
                std::string::npos &&
            d.message.find("2.13") != std::string::npos)
            return true;
    }
    return false;
}

bool hasAnyError(const OwnershipResult& r) {
    return r.diagnostics.hasErrors();
}

// A `Type`-typed parameter reached through a method FORMAL. `Type` is
// runtime-managed (§28.9), so a plain parameter is a borrow and `owned Type` is
// inadmissible — the parameter is the spec's own idiom, not a workaround.
std::string viaTypeParam(const std::string& member) {
    return "module M; "
           "class C "
           "  public static method Take(t: Type): int "
           "    var _x := t." + member + "; "
           "    return 0; "
           "  end Take; "
           "  public static method Main(): int return 0; end Main; "
           "end C;";
}

}  // namespace

// ---------------------------------------------------------------------------
// R1 — the defect itself. Fix #5791 / stdlib §2.13.
// An unknown member on a `Type`-typed value must be REJECTED with the §2.13
// closed-surface diagnostic, not compiled at exit 0.
// ---------------------------------------------------------------------------
BEZEL_TEST(type_surface_unknown_member_5791,
           unknown_member_on_type_param_rejected_spec_2_13) {
    auto r = parseAndCheckOwnership(viaTypeParam("TotallyBogusMember"));
    BEZEL_EXPECT_TRUE(hasAnyError(r));
    BEZEL_EXPECT_TRUE(
        isTypeSurfaceReject(r, "TotallyBogusMember"));
}

// R2 — the §2.13 surface is CLOSED, so a name that is a plausible reflection
// spelling (§5.18: "member reflection is the largest possible breach") but is
// deliberately NOT provided is also rejected. `GetMember` is the §2.13
// "What `Type` deliberately does not provide" paragraph stated as code.
// Fix #5791 / stdlib §2.13 / rationale §5.18.
BEZEL_TEST(type_surface_unknown_member_5791,
           by_name_reflection_member_rejected_spec_2_13) {
    auto r = parseAndCheckOwnership(viaTypeParam("GetMember"));
    BEZEL_EXPECT_TRUE(hasAnyError(r));
    BEZEL_EXPECT_TRUE(isTypeSurfaceReject(r, "GetMember"));
}

// R3 — the `?.` form reaches the SAME closed surface (it asks the same
// predicate, `closedValueSurfaceMemberReason`, as the `.` form). Without this
// row the fix could silently cover only the `.` spelling.
// Fix #5791 / stdlib §2.13.
BEZEL_TEST(type_surface_unknown_member_5791,
           unknown_member_on_nullable_type_nullcond_rejected_spec_2_13) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static method Take(t: Type?): int "
        "    var _x := t?.Bogus; "
        "    return 0; "
        "  end Take; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasAnyError(r));
    BEZEL_EXPECT_TRUE(isTypeSurfaceReject(r, "Bogus"));
}

// R4 — the `Type` FIELD spelling. A field is a second receiver shape carrying
// the same resolved type, and a receiver-shaped gate that only fires on
// parameters would leave it silently accepted.
// Fix #5791 / stdlib §2.13.
BEZEL_TEST(type_surface_unknown_member_5791,
           unknown_member_on_type_field_rejected_spec_2_13) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public _t: Type; "
        "  public method Probe(): int "
        "    var _x := _t.Bogus; "
        "    return 0; "
        "  end Probe; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasAnyError(r));
    BEZEL_EXPECT_TRUE(isTypeSurfaceReject(r, "Bogus"));
}

// ---------------------------------------------------------------------------
// THE ACCEPTANCE SIDE. A reject-flip is only correct if it rejects NOTHING
// that stdlib §2.13 declares. Each row below names one §2.13 member and
// requires it to compile CLEAN (no error at all). If the surface list ever
// loses an entry, the corresponding row goes red — this is the half of the
// fix that catches a would-false-reject, which a reject-only test cannot.
//
// The fifteen names are §2.13's declaration block, transcribed mechanically:
// `Name`, `FullName`, the nine kind predicates, `BaseType`,
// `IsAssignableFrom`, `EnumNames`, `EnumValues`.
// ---------------------------------------------------------------------------

// R5 — the two rendering properties (§2.13, #1812): "BankAccount" /
// "BankingApp.BankAccount".
BEZEL_TEST(type_surface_unknown_member_5791,
           spec_members_name_fullname_accepted_spec_2_13) {
    for (const char* m : {"Name", "FullName"}) {
        auto r = parseAndCheckOwnership(viaTypeParam(m));
        BEZEL_EXPECT_TRUE(!hasAnyError(r));
    }
}

// R6 — the nine kind predicates (§2.13: "Exactly one of IsClass, IsInterface,
// IsRecord, IsEnum, IsChoice, IsPrimitive, IsInlineArray, IsFunctionType is
// true for any Type").
BEZEL_TEST(type_surface_unknown_member_5791,
           spec_members_kind_predicates_accepted_spec_2_13) {
    for (const char* m : {"IsClass", "IsInterface", "IsRecord", "IsEnum",
                          "IsChoice", "IsPrimitive", "IsInlineArray",
                          "IsFunctionType", "IsValueType"}) {
        auto r = parseAndCheckOwnership(viaTypeParam(m));
        BEZEL_EXPECT_TRUE(!hasAnyError(r));
    }
}

// R7 — the lattice queries and the enum-metadata views. `IsAssignableFrom` and
// `EnumNames`/`EnumValues` are the three members the FIRST attempt at this fix
// omitted (its arm typed only twelve names and its `else` would have rejected
// these three). They are spec-declared and must be accepted.
// Fix #5791 / stdlib §2.13.
BEZEL_TEST(type_surface_unknown_member_5791,
           spec_members_lattice_and_enum_views_accepted_spec_2_13) {
    for (const char* m : {"BaseType", "IsAssignableFrom", "EnumNames",
                          "EnumValues"}) {
        auto r = parseAndCheckOwnership(viaTypeParam(m));
        BEZEL_EXPECT_TRUE(!hasAnyError(r));
    }
}

// R8 — §8.1's universal members and §8.7's `Dispose` exist on `Type` (an
// ordinary class) and are handled by their OWN upstream gates (#2457, §28.6,
// the §8.1 arity check). A closed-surface reject that shadowed them would
// replace a precise diagnostic with a generic one — and would be a
// behaviour flip on rows that already pin those messages.
// Fix #5791 / stdlib §2.13 + language spec §8.1 / §8.7.
BEZEL_TEST(type_surface_unknown_member_5791,
           universal_object_members_not_shadowed_spec_2_13) {
    // Bare references, so the assertion is about the closed surface only and
    // not about a call's arity or a borrow's ownership.
    for (const char* m : {"Dispose", "ReferenceEquals"}) {
        auto r = parseAndCheckOwnership(viaTypeParam(m));
        BEZEL_EXPECT_TRUE(!isTypeSurfaceReject(r, m));
    }
}

// R9 — SHADOWING. §2.3: every always-available name may be shadowed by a local
// declaration, so a user may declare `class Type`. A gate keyed on the SPELLING
// would reject that user's real members; the fix keys on the resolved type
// (a `Named` TypeRef with a non-null `declaration`), so the user's `Type` keeps
// its own surface and its own §11.3 "no member" wording.
// Fix #5791 / §2.3 + §11.3.
BEZEL_TEST(type_surface_unknown_member_5791,
           user_declared_type_class_keeps_own_surface_spec_2_3) {
    auto ok = parseAndCheckOwnership(
        "module M; "
        "class Type public _v: int; "
        "  public method Ok(): int return 1; end Ok; "
        "end Type; "
        "class C "
        "  public static method Take(t: Type): int "
        "    var _x := t.Ok(); "
        "    return _x; "
        "  end Take; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasAnyError(ok));

    // And an unknown member on the USER's `Type` still rejects — with §11.3's
    // wording, not §2.13's, because §2.13 does not describe it.
    auto bad = parseAndCheckOwnership(
        "module M; "
        "class Type public _v: int; end Type; "
        "class C "
        "  public static method Take(t: Type): int "
        "    var _x := t.Bogus; "
        "    return 0; "
        "  end Take; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasAnyError(bad));
    BEZEL_EXPECT_TRUE(!isTypeSurfaceReject(bad, "Bogus"));
}

// R10 — SHADOWING, the other direction. A generic type PARAMETER named `Type`
// exposes only its constraints (§3.6 rule 2 / §14.2), not §2.13's surface, and
// is rejected by the upstream `isInScopeTypeParam` gate — never by this fix.
// Requiring that exact wording is what proves the two gates do not overlap.
// Fix #5791 / §3.6 rule 2 + §14.2.
BEZEL_TEST(type_surface_unknown_member_5791,
           type_parameter_named_type_is_not_the_builtin_spec_3_6) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static method Take<Type>(t: Type): int "
        "    var _x := t.Bogus; "
        "    return 0; "
        "  end Take; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasAnyError(r));
    // The upstream gate's wording, NOT §2.13's — and specifically NOT a §2.13
    // reject, which would mean this fix captured a type parameter.
    BEZEL_EXPECT_TRUE(!isTypeSurfaceReject(r, "Bogus"));
}