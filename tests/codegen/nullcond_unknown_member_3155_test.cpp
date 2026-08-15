// Sema — issue #3155 (§7.5, v0.255). The `?.` form must MIRROR the `.` form's
// member-not-found diagnostic, not silently fall through. Before this fix,
// every `x?.M` whose `M` was not a real member of `x`'s type (and not a §26.5
// extension, and not a built-in value member) returned without setting a type
// AND without emitting any error: the NullCondMemberExpr was left untyped, the
// enclosing local initializer was typed to nothing, and `lowerLocalDecl`
// (LlvmBackend_Setup.cpp) degraded to a zero-initialized slot — emitting only
// the "internal: lowerLocalDecl's initializer expression failed to lower to a
// value for this local's slot" note (#2434). Worse than the accept: the
// resulting *value* was wrong, because `x?.M ?? d` produced neither the
// member nor the coalesce default.
//
// The plain `.` form (resolveMemberAccess) rejects this in two complementary
// sites: a primitive-typed receiver (no TypeSymbol) draws "no member `M` on
// primitive type `T`" with the documented-members-and-extension-no-match
// tail; a known-complete user type (class/record/enum/choice with a non-null
// decl or a non-generic imported decl) draws "no member `M` on type `T`
// (§11.3, §12.1)". The two are distinct because primitive value types have
// no user TypeSymbol and so miss the known-complete gate. The `?.` form's
// resolveNullCondMember function previously had NEITHER site — the `if
// (!member) return;` point just fell through silently, and codegen ran into
// the lowerLocalDecl degraded-zero shape.
//
// SPEC: §7.5 (null-conditional member access — "evaluates to null if the
// receiver is null; otherwise the member is evaluated normally"), §11.3/§12.1
// (the member-lookup reject used for the user-type form), stdlib §9 / §8.1
// (the documented-members/extension-no-match tail used for the primitive
// form).
//
// Every case drives the REAL `bezelc` AOT path via runAot. For a diagnostic
// fix, "it compiled" / "exit 0" are NOT oracles: without the fix the
// discriminator cases compile PAST sema and lowerLocalDecl reports an
// internal-only note (#2434). So each discriminator asserts BOTH that the
// compile was rejected AND that the expected `?.` / `.` member-not-found
// message TEXT is present — a diagnostic can be right to FIRE and wrong in
// what it SAYS (assert TEXT). The controls assert (a) the `?.` form's new
// reject is ABSENT on valid members (false-reject guard) and (b) the
// character-identical `.` form keeps its own reject (regression guard — the
// fix must not silently weaken the `.` path).

#include <string>
#include <string_view>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// runAot returns a JitResult whose `.diagnostics` holds the captured compiler
// stderr on a reject. `rejectsAotWith` asserts BOTH that the compile did not
// run AND that the needle (the member-not-found wording) is in an Error
// diagnostic.
bool rejectsAotWith(const std::string& program, std::string_view needle) {
    auto r = runAot(program);
    if (r.ran) return false;  // not rejected — ran to completion
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find(std::string(needle)) != std::string::npos)
            return true;
    }
    return false;
}

// Inverse of rejectsAotWith for control cases: the program MAY compile and
// run, MAY be rejected for an unrelated reason, but the specific needle must
// be ABSENT from every diagnostic — a false-reject guard.
bool noUnknownMemberError(const std::string& program,
                          std::string_view needle) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find(std::string(needle)) != std::string::npos)
            return false;
    }
    return true;
}

// A user class with one real member (`Real`) and a destructor, used as the
// receiver in the class-typed discriminator. The class has a non-null `decl`
// (it's declared locally), so it qualifies as a `knownCompleteUserType`.
const char* kUserClass =
    "public class C "
    "  public constructor() end constructor; "
    "  public method Real(): int => 1; "
    "  destructor end destructor; "
    "end C; ";

// A user record with one real member (`Real`) — used as the record-typed
// discriminator. Records are `knownCompleteUserType` per #795/§11.3.
const char* kUserRecord =
    "record R public property Real: int; "
    "public constructor() Real := 1; end constructor; end R; ";

std::string wrap(const std::string& prelude, const std::string& body) {
    return "module M3155; " + prelude +
           "class W public static method Main() " + body +
           " end Main; end W;";
}

}  // namespace

// ===========================================================================
// Discriminators — FAIL without the fix (silent accept + degraded-zero
// lowering), PASS with it (sema rejects with the `?.` member-not-found
// message).
// ===========================================================================

// `?.Bogus` on a non-nullable `int` — the issue's Repro A. Without the fix:
// silently accepts, lowerLocalDecl degrades to 0. With the fix: rejected with
// the primitive-type wording, mirroring the `.` form's identical message.
BEZEL_TEST(codegen_nullcond_unknown_member_3155,
           nullcond_bogus_on_int_rejected_spec_7_5) {
    BEZEL_EXPECT_TRUE(rejectsAotWith(
        wrap("",
             "var n: int := 1; "
             "var a := n?.Bogus; "),
        "no member `Bogus` on primitive type `int`"));
}

// `?.Bogus` on a non-nullable `string` — string has its own closed surface
// (§9.6) and the `.` form rejects an unknown name with the
// "string member `.X` is not yet supported in codegen" wording; the `?.` form
// must mirror it (only the operator character changes — `.` → `?.`).
BEZEL_TEST(codegen_nullcond_unknown_member_3155,
           nullcond_bogus_on_string_rejected_spec_7_5) {
    BEZEL_EXPECT_TRUE(rejectsAotWith(
        wrap("",
             "var s: string := \"hi\"; "
             "var a := s?.Bogus; "),
        "string member `?.Bogus` is not yet supported in codegen"));
}

// `?.Bogus` on a nullable `string?` — same wording as the non-nullable case;
// §7.5's null short-circuit is a runtime property, not a static one, so the
// member-availability gate fires identically.
BEZEL_TEST(codegen_nullcond_unknown_member_3155,
           nullcond_bogus_on_nullable_string_rejected_spec_7_5) {
    BEZEL_EXPECT_TRUE(rejectsAotWith(
        wrap("",
             "var s: string? := null; "
             "var a := s?.Bogus; "),
        "string member `?.Bogus` is not yet supported in codegen"));
}

// `?.Bogus` on a nullable class receiver (`C?` where C has no member named
// Bogus). This is the issue's Repro B — the user-type gate fires, citing
// §11.3/§12.1, mirroring the `.` form's wording for a class receiver.
BEZEL_TEST(codegen_nullcond_unknown_member_3155,
           nullcond_bogus_on_nullable_class_rejected_spec_7_5) {
    BEZEL_EXPECT_TRUE(rejectsAotWith(
        wrap(std::string(kUserClass),
             "var q: C? := null; "
             "var a := q?.Bogus; "),
        "no member `Bogus` on type `C`"));
}

// `?.Bogus` on a non-nullable class receiver (`C` where C has no member named
// Bogus). Both forms reject it; the `.` form already does, the `?.` form now
// does too.
BEZEL_TEST(codegen_nullcond_unknown_member_3155,
           nullcond_bogus_on_non_nullable_class_rejected_spec_7_5) {
    BEZEL_EXPECT_TRUE(rejectsAotWith(
        wrap(std::string(kUserClass),
             "using var c := new C(); "
             "var a := c?.Bogus; "),
        "no member `Bogus` on type `C`"));
}

// `?.Bogus` on a nullable record receiver — record kind is also a
// knownCompleteUserType per §11.3/§12.1.
BEZEL_TEST(codegen_nullcond_unknown_member_3155,
           nullcond_bogus_on_nullable_record_rejected_spec_7_5) {
    BEZEL_EXPECT_TRUE(rejectsAotWith(
        wrap(std::string(kUserRecord),
             "var r: R? := null; "
             "var a := r?.Bogus; "),
        "no member `Bogus` on type `R`"));
}

// ===========================================================================
// Controls — PASS both ways (pre-empt). The `?.` reject must NOT fire on a
// VALID member, and the character-identical `.` form must keep its OWN
// reject intact (regression guard for the parallel mirror — the fix must
// not weaken the `.` path).
// ===========================================================================

// `?.Real` on a `C?` receiver — `Real` IS a real method of C, so the access
// is valid. The `?.` reject must NOT fire (false-reject guard).
BEZEL_TEST(codegen_nullcond_unknown_member_3155,
           nullcond_real_on_nullable_class_accepted_control_spec_7_5) {
    BEZEL_EXPECT_TRUE(noUnknownMemberError(
        wrap(std::string(kUserClass),
             "var q: C? := null; "
             "var a := q?.Real(); "),
        "no member `Real`"));
}

// `?.Bogus` on an `int` receiver — the `.` form's identical reject must
// REMAIN (regression guard). Without this guard, the mirror fix could
// silently weaken the `.` path.
BEZEL_TEST(codegen_nullcond_unknown_member_3155,
           dot_bogus_on_int_still_rejected_control_spec_7_5) {
    BEZEL_EXPECT_TRUE(rejectsAotWith(
        wrap("",
             "var n: int := 1; "
             "var a := n.Bogus; "),
        "no member `Bogus` on primitive type `int`"));
}

// `?.Bogus` on a `string` receiver — the `.` form's "string member" reject
// must REMAIN (regression guard for the string-receiver arm of the mirror).
BEZEL_TEST(codegen_nullcond_unknown_member_3155,
           dot_bogus_on_string_still_rejected_control_spec_7_5) {
    BEZEL_EXPECT_TRUE(rejectsAotWith(
        wrap("",
             "var s: string := \"hi\"; "
             "var a := s.Bogus; "),
        "string member `.Bogus` is not yet supported in codegen"));
}

// `?.Length` on a `string?` receiver — the §9.6 built-in value member must
// STILL resolve (false-reject guard for the string-typed arm: the
// builtinValueMemberType branch above sets the type and returns BEFORE the
// string-typed gate fires).
BEZEL_TEST(codegen_nullcond_unknown_member_3155,
           nullcond_length_on_nullable_string_accepted_control_spec_7_5) {
    BEZEL_EXPECT_TRUE(noUnknownMemberError(
        wrap("",
             "var s: string? := null; "
             "var n := s?.Length; "),
        "string member `?.Length`"));
}