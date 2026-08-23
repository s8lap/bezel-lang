// Spec-derived regression tests for issue #3331 — three documented
// conditional members had no enforcement found anywhere in the compiler.
// The proposal-0116 census (`docs/proposals/0116-conditional-members.md`
// §2, "C2/C4/C5") named each, and the spec landed the necessary
// vocabulary at v0.277 (stdlib §4.14, §4.15, §5; spec §14.2 register).
// The COMPILER carries the vocabulary but did not enforce it; the probes
// below pin the missing enforcement on HEAD.
//
// Authority:
//   C2 — stdlib §4.14 `DangerousPointer()`: "admitted only where T is in
//        the FFI-pointee set … every other T … is rejected at the call's
//        instantiation with a diagnostic naming the set." Per proposal
//        0116 D8 the gate is the v0.283 subtraction-review loud-reject
//        stub, not a per-T gate — both T-in-set and T-out-of-set are
//        rejected with a citation-naming diagnostic.
//   C4 — stdlib §4.14 "No materialization on this family (v0.277). … The
//        compiler never admitted the call: the span family's member
//        surface is a closed allow-list ending in a 'no member' diagnostic
//        that enumerates the surface and omits the name." REGRESSION
//        TEST: the rejection exists and cites §4.14/§4.15.
//   C5 — stdlib §5 + proposal 0116 D9: the six hashable-element
//        operators (`Distinct`, `Union`, `Intersect`, `Except`, `Contains`,
//        `ToSet`) move to `extension IEnumerable<T> where T: hashable`
//        (the constrained extension block, v0.277). The CONSTRAINT must
//        actually filter applicability — proposal 0116 flags #3336 as the
//        keystone that "§26.4's block `where` clause does not filter
//        extension applicability" and #3331 as the unenforced-condition
//        instance of it. The pre-fix compiler silently accepts a call
//        with a non-hashable T (probe_c5 / probe_c5_toSet / probe_c5_
//        contains / probe_c5_union all compile clean on HEAD); the fix
//        rejects with a §14.2 `hashable` diagnostic naming the operator.
//
// Axis grid:
//   C2 receiver: OwningBuffer<int> | OwningBuffer<string> | OwningBuffer<Point>
//        both T-in-FFI-pointee-set and T-out-of-set admit the same loud
//        reject (proposal 0116 D8: the gate is the v0.283 subtraction-
//        review stub, not the per-T gate)
//   C4 receiver: ReadOnlySpan<int> | ReadOnlySpan<Point>
//        the call is rejected regardless of T — the member does not exist
//   C5 operator: Distinct | Union | Intersect | Except | Contains | ToSet
//      C5 receiver: Array<Holder> (Holder non-hashable per §9.3)
//                    Array<int>    (int is hashable; control — accepted)
//                    Set<Holder>   (non-hashable; rejected)
//                    Set<int>      (control — accepted)
//
// Verbatim rule citations (per CLAUDE.md §3, "Trace before writing"):
//   C2 stdlib §4.14: "Declared on the value-typed presentation only,
//        and admitted only where T is in the FFI-pointee set […] every
//        other T […] is rejected at the call's instantiation with a
//        diagnostic naming the set."
//   C4 stdlib §4.14: "No materialization on this family (v0.277). …
//        The compiler never admitted the call."
//   C5 stdlib §5:   "The six operators that require hashable T are
//        declared in their own constrained block (v0.277). The
//        requirement is an ordinary constraint (language spec §14.2) on
//        a §26.4 extension block, not a conditionally-admitted member."

#include <string>
#include <string_view>

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

using bezel::test::SemaResult;

SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// ─── C2 DIAGNOSTIC HELPERS ──────────────────────────────────────────────────

// C2 stdlib §4.14 + proposal 0116 D8: the call site is rejected with a
// diagnostic naming §4.14 (the stdlib section that documents the
// conditional member) AND the v0.283 subtraction-review status (D8's
// discipline). The pre-fix path emits the GENERIC #2457 catch-all, which
// names neither section nor the subtraction-review status — so pinning
// on "section 4.14" + "v0.283" together closes the regression.
bool hasDangerousPointerSectionDiagnostic(const SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("DangerousPointer") == std::string::npos) continue;
        if (d.message.find("4.14") == std::string::npos) continue;
        return true;
    }
    return false;
}

// ─── C4 DIAGNOSTIC HELPERS ──────────────────────────────────────────────────

// C4 stdlib §4.14: the call is rejected with a diagnostic that
// enumerates the surface and OMITS `ToArray`. Pinning the wording — not
// just `hasErrors()` — is the regression-test discipline (CLAUDE.md
// §1.3): a generic "no member" path that names the operator would still
// flip `hasErrors()` but would miss the surface-enumeration wording the
// spec requires (and any future path that ADDS `ToArray` to the surface
// would also flip it).
bool hasReadOnlySpanNoToArrayDiagnostic(const SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("ReadOnlySpan") == std::string::npos) continue;
        if (d.message.find("ToArray") == std::string::npos) continue;
        // The wording enumerates the SURFACE and OMITS ToArray. A
        // generic "no member" reject names the missing member but does
        // not enumerate the surrounding surface; a regression that
        // restores ToArray to the surface still flips hasErrors() but
        // stops flipping the surface-enumeration wording.
        if (d.message.find("Length") == std::string::npos) continue;
        if (d.message.find("IsEmpty") == std::string::npos) continue;
        if (d.message.find("Slice") == std::string::npos) continue;
        return true;
    }
    return false;
}

// ─── C5 DIAGNOSTIC HELPERS ──────────────────────────────────────────────────

// C5 stdlib §5 + proposal 0116 D9: the rejected call carries a §14.2
// `hashable` diagnostic. The probe source spells one of the six
// hashable-requiring operators, so the diagnostic must name BOTH the
// operator (so the user knows which call is the problem) AND the
// `hashable` constraint (so the user knows which property to fix).
bool hasLinqHashableDiagnostic(const SemaResult& r,
                               std::string_view operatorName) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(std::string(operatorName)) == std::string::npos)
            continue;
        if (d.message.find("hashable") == std::string::npos) continue;
        return true;
    }
    return false;
}

}  // namespace

// ============================================================================
// C2 — OwningBuffer<T>.DangerousPointer() (#3331, stdlib §4.14)
// ============================================================================

// C2 (probe): OwningBuffer<int>.DangerousPointer() — T = int IS in the
// FFI-pointee set, so the per-T predicate (when landed) would admit the
// call. Per proposal 0116 D8, however, the COMPILER has no gate at all
// — neither the per-T predicate nor a loud-reject stub — and the
// v0.283 subtraction-review status is the documented disposition. The
// gate the fix installs is the loud-reject stub (named section +
// subtraction-review status) for ALL T.
BEZEL_TEST(conditional_member_enforcement_3331,
           owning_buffer_int_dangerous_pointer_rejected_spec_4_14) {
    auto r = parseSemaAll(
        "module M; "
        "class C public static method Main(): int "
        "  using var b := new OwningBuffer<int>(5); "
        "  var p := b.DangerousPointer(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDangerousPointerSectionDiagnostic(r));
}

// C2 (probe): OwningBuffer<Point>.DangerousPointer() — T = Point (a
// positional record) is NOT in the FFI-pointee set. The per-T predicate
// would reject with the FFI-pointee-set diagnostic; the loud-reject stub
// (the v0.283-subtraction-review gate, per D8) rejects unconditionally.
// Either gate rejects the call; the diagnostic wording must cite §4.14.
BEZEL_TEST(conditional_member_enforcement_3331,
           owning_buffer_record_dangerous_pointer_rejected_spec_4_14) {
    auto r = parseSemaAll(
        "module M; "
        "public record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var b := new OwningBuffer<Point>(5); "
        "  var p := b.DangerousPointer(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDangerousPointerSectionDiagnostic(r));
}

// C2 (probe): OwningBuffer<string>.DangerousPointer() — T = string is
// NOT in the FFI-pointee set. Same gate as the record case; pinning on
// §4.14 wording.
BEZEL_TEST(conditional_member_enforcement_3331,
           owning_buffer_string_dangerous_pointer_rejected_spec_4_14) {
    auto r = parseSemaAll(
        "module M; "
        "class C public static method Main(): int "
        "  using var b := new OwningBuffer<string>(5); "
        "  var p := b.DangerousPointer(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDangerousPointerSectionDiagnostic(r));
}

// ============================================================================
// C4 — ReadOnlySpan<T>.ToArray() (#3331, stdlib §4.14)
// ============================================================================

// C4 (regression): the v0.277 deletion of the member is the gate. A
// call to `s.ToArray()` on a `ReadOnlySpan<int>` is rejected with a
// "no member" diagnostic that enumerates the documented surface and
// OMITS `ToArray` — pinning the wording catches a regression that adds
// the member back (v0.222 #1810 introduced it once and v0.277 deleted
// it; the section's prose calls the absence a recorded decision rather
// than a gap, so the wording is part of the contract).
BEZEL_TEST(conditional_member_enforcement_3331,
           readonly_span_int_to_array_rejected_spec_4_14) {
    auto r = parseSemaAll(
        "module M; "
        "class C public static method Main(): int "
        "  using var b := new OwningBuffer<int>(5); "
        "  var s := b.AsReadOnlySpan(); "
        "  var a := s.ToArray(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasReadOnlySpanNoToArrayDiagnostic(r));
}

// C4 (regression): same on a value-typed record receiver — the surface
// is the same regardless of T, so the rejection wording must be the
// same. Pinning both surfaces closes the "re-add ToArray" regression
// even if someone special-cases it for a single T.
BEZEL_TEST(conditional_member_enforcement_3331,
           readonly_span_record_to_array_rejected_spec_4_14) {
    auto r = parseSemaAll(
        "module M; "
        "public record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var b := new OwningBuffer<Point>(5); "
        "  var s := b.AsReadOnlySpan(); "
        "  var a := s.ToArray(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasReadOnlySpanNoToArrayDiagnostic(r));
}

// ============================================================================
// C5 — §5 hashable-element LINQ operators (#3331, stdlib §5 + D9)
// ============================================================================

// A non-hashable T for Array<T> membership probes: a positional record
// whose class-typed field leaves it non-hashable per §9.3 (the
// "Holder" shape that hashable_constraint_2087_test.cpp:336 uses to
// prove the §14.2 type-instantiation reject on the sibling surface).
// Sharing the shape across C5 tests keeps the failure surface
// homogeneous — every "rejected" case below differs only in operator
// name, not in receiver.

// C5 (probe): `xs.Distinct()` on Array<Holder> — a lazy LINQ operator
// (per §5 the operator is deferred). Pre-fix the call compiles cleanly;
// post-fix it is rejected with a §14.2 `hashable` diagnostic naming
// `Distinct`.
BEZEL_TEST(conditional_member_enforcement_3331,
           array_holder_distinct_rejected_spec_5_14_2) {
    auto r = parseSemaAll(
        "module M; "
        "class R public V: int := 0; destructor end destructor; end R; "
        "record Holder(item: R); "
        "class C public static method Main(): int "
        "  using var xs := new Array<Holder>(); "
        "  var ys := xs.Distinct(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasLinqHashableDiagnostic(r, "Distinct"));
}

// C5 (probe): `xs.Union(other)` on Array<Holder> — another lazy LINQ
// operator, same constraint. Pre-fix compiles; post-fix rejected.
BEZEL_TEST(conditional_member_enforcement_3331,
           array_holder_union_rejected_spec_5_14_2) {
    auto r = parseSemaAll(
        "module M; "
        "class R public V: int := 0; destructor end destructor; end R; "
        "record Holder(item: R); "
        "class C public static method Main(): int "
        "  using var xs := new Array<Holder>(); "
        "  using var ys := new Array<Holder>(); "
        "  var zs := xs.Union(ys); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasLinqHashableDiagnostic(r, "Union"));
}

// C5 (probe): `xs.Intersect(other)` on Array<Holder> — same family.
// Pre-fix compiles; post-fix rejected.
BEZEL_TEST(conditional_member_enforcement_3331,
           array_holder_intersect_rejected_spec_5_14_2) {
    auto r = parseSemaAll(
        "module M; "
        "class R public V: int := 0; destructor end destructor; end R; "
        "record Holder(item: R); "
        "class C public static method Main(): int "
        "  using var xs := new Array<Holder>(); "
        "  using var ys := new Array<Holder>(); "
        "  var zs := xs.Intersect(ys); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasLinqHashableDiagnostic(r, "Intersect"));
}

// C5 (probe): `xs.Except(other)` on Array<Holder> — same family.
BEZEL_TEST(conditional_member_enforcement_3331,
           array_holder_except_rejected_spec_5_14_2) {
    auto r = parseSemaAll(
        "module M; "
        "class R public V: int := 0; destructor end destructor; end R; "
        "record Holder(item: R); "
        "class C public static method Main(): int "
        "  using var xs := new Array<Holder>(); "
        "  using var ys := new Array<Holder>(); "
        "  var zs := xs.Except(ys); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasLinqHashableDiagnostic(r, "Except"));
}

// C5 (probe): `xs.Contains(item)` on Array<Holder> — a TERMINAL LINQ
// operator (per §5, returns bool). Pre-fix compiles cleanly; post-fix
// rejected with a §14.2 `hashable` diagnostic naming `Contains`.
BEZEL_TEST(conditional_member_enforcement_3331,
           array_holder_contains_rejected_spec_5_14_2) {
    auto r = parseSemaAll(
        "module M; "
        "class R public V: int := 0; destructor end destructor; end R; "
        "record Holder(item: R); "
        "class C public static method Main(): int "
        "  using var xs := new Array<Holder>(); "
        "  var b := xs.Contains(new Holder(new R())); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasLinqHashableDiagnostic(r, "Contains"));
}

// C5 (probe): `using var s := xs.ToSet()` on Array<Holder> — a
// MATERIALIZER (per §5, returns owned Set<T>). Pre-fix compiles
// cleanly; post-fix rejected.
BEZEL_TEST(conditional_member_enforcement_3331,
           array_holder_to_set_rejected_spec_5_14_2) {
    auto r = parseSemaAll(
        "module M; "
        "class R public V: int := 0; destructor end destructor; end R; "
        "record Holder(item: R); "
        "class C public static method Main(): int "
        "  using var xs := new Array<Holder>(); "
        "  using var s := xs.ToSet(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasLinqHashableDiagnostic(r, "ToSet"));
}

// C5 (probe): same six operators on a Set<Holder> receiver — the
// "containing collection is itself a hash container" axis. A Set<Holder>
// instantiation is itself rejected (per §4.10 / §3.6 rule 2), but a
// `s.ToSet()` on an `s: Set<Holder>` reads §5's `extension Set<T>` (no
// such block — Set<T> conforms to IEnumerable<T> and reads §5's
// IEnumerable block). Whatever the receiver shape, the §5 operator on
// a non-hashable T is rejected with the same hashable diagnostic.
BEZEL_TEST(conditional_member_enforcement_3331,
           set_holder_to_set_rejected_spec_5_14_2) {
    auto r = parseSemaAll(
        "module M; "
        "class R public V: int := 0; destructor end destructor; end R; "
        "record Holder(item: R); "
        "class C public static method Main(): int "
        "  using var s := new Set<Holder>(); "
        "  s.Add(new Holder(new R())); "
        "  using var t := s.ToSet(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasLinqHashableDiagnostic(r, "ToSet"));
}

// ─── C5 control axis: every operator on a hashable T still resolves ─────

// C5 control: every §5 hashable operator on Array<int> (int is
// auto-hashable, §3.6) resolves cleanly — the §14.2 constraint is
// satisfied, the call is typed, the program compiles. This is the
// "no false positive" axis of the regression test: the fix must
// reject ONLY non-hashable T, never a hashable one.
BEZEL_TEST(conditional_member_enforcement_3331,
           array_int_distinct_accepted_spec_5) {
    auto r = parseSemaAll(
        "module M; "
        "class C public static method Main(): int "
        "  using var xs := new Array<int>(); "
        "  xs.Add(1); xs.Add(2); xs.Add(1); "
        "  var ys := xs.Distinct(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// C5 control: `xs.ToSet()` on Array<int> resolves — `Set<int>` is
// well-formed, the materializer is typed, the program compiles.
BEZEL_TEST(conditional_member_enforcement_3331,
           array_int_to_set_accepted_spec_5) {
    auto r = parseSemaAll(
        "module M; "
        "class C public static method Main(): int "
        "  using var xs := new Array<int>(); "
        "  xs.Add(1); xs.Add(2); "
        "  using var s := xs.ToSet(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// C5 control: `xs.Contains(0)` on Array<int> — the terminal operator
// on a hashable T is accepted.
BEZEL_TEST(conditional_member_enforcement_3331,
           array_int_contains_accepted_spec_5) {
    auto r = parseSemaAll(
        "module M; "
        "class C public static method Main(): int "
        "  using var xs := new Array<int>(); "
        "  xs.Add(1); xs.Add(2); "
        "  var b: bool := xs.Contains(0); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}