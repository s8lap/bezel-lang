// Spec-derived tests for fix #4676 (stdlib §6.5 + §12.9, v0.307):
// EventSource<T> and SharedEventSource<T> are classes and therefore INVARIANT
// in their payload type. The variance check at checkAssignable.cpp:4507 already
// rejects a cross-payload assignment per §12.9; the user-facing diagnostic was
// the generic §5.1 "not implicitly convertible" wording, which did not name the
// source-class invariance — the conformance case l6_5_0014.bzl passed vacuously
// because the rule's "invariant" substring was nowhere in the diagnostic.
//
// This test pins the new diagnostic that names the source-class invariance
// specifically. It uses `headName()` (not `declaration->name`) because
// EventSource / SharedEventSource are stdlib builtins — their `declaration`
// field is null and the type name lives in `importedName` instead
// (src/sema/TypeRef.hpp:80 headName()).

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff any ERROR diagnostic carries the new #4676 invariance needle.
// Pins THIS fix (the named-diagnostic for the source-class invariance),
// not merely "some error".
bool hasInvarianceDiagnostic(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("invariant in their payload type") != std::string::npos)
            return true;
    return false;
}

// True iff any ERROR diagnostic carries the GENERIC §5.1 wording — the
// pre-fix wording the conformance case l6_5_0014.bzl's "invariant" substring
// used to miss. Control cases on non-source-class heads assert this so a
// regression of the new diagnostic back to the generic wording is caught.
bool hasGenericNotConvertible(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("not implicitly convertible") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ---- FAIL-without / PASS-with: source-class invariance gets the NAMED diagnostic ----

// (1) The conformance case l6_5_0014's exact shape — `EventSource<IAnimal>`
// initialized from `new EventSource<Dog>()` where `Dog: IAnimal`. Pre-fix the
// variance check fires but the diagnostic is the generic §5.1 wording and the
// `invariant in their payload type` substring is absent. Post-fix the named
// diagnostic fires and `hasInvarianceDiagnostic()` returns true.
BEZEL_TEST(event_source_variance_diagnostic_4676, eventsource_ianimal_from_dog_rejected_with_named_diagnostic) {
    auto r = parseSemaAll(
        "module M;\n"
        "public interface IAnimal\n"
        "  method Speak(): string;\n"
        "end IAnimal;\n"
        "public class Dog: IAnimal\n"
        "  public constructor() end constructor;\n"
        "  public method Speak(): string => \"woof\";\n"
        "end Dog;\n"
        "public class Program\n"
        "  public method M()\n"
        "    using var src: EventSource<IAnimal> := new EventSource<Dog>();\n"
        "  end M;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInvarianceDiagnostic(r));
}

// (2) Mirror of (1) with `SharedEventSource<T>` — the spec treats both source
// classes identically (§6.5: "the source classes are invariant"), so the
// diagnostic must name BOTH. Pre-fix this was missed because
// `SharedEventSource` is the same builtin-name shape as `EventSource` (no
// declaration, type name in importedName).
BEZEL_TEST(event_source_variance_diagnostic_4676, sharedeventsource_ianimal_from_dog_rejected_with_named_diagnostic) {
    auto r = parseSemaAll(
        "module M;\n"
        "public interface IAnimal\n"
        "  method Speak(): string;\n"
        "end IAnimal;\n"
        "public class Dog: IAnimal\n"
        "  public constructor() end constructor;\n"
        "  public method Speak(): string => \"woof\";\n"
        "end Dog;\n"
        "public class Program\n"
        "  public method M()\n"
        "    using var src: SharedEventSource<IAnimal> := new SharedEventSource<Dog>();\n"
        "  end M;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInvarianceDiagnostic(r));
}

// (3) Cross-payload-type primitives (`int` vs `long`) — exercises the same
// invariance rule with non-class payloads. `int :> long` is widening, so
// `EventSource<int>` is not assignable from `EventSource<long>` even though
// the inner type widens (the source class is INVARIANT in its payload).
BEZEL_TEST(event_source_variance_diagnostic_4676, eventsource_int_from_long_rejected_with_named_diagnostic) {
    auto r = parseSemaAll(
        "module M;\n"
        "public class Program\n"
        "  public method M()\n"
        "    var dst: EventSource<int> := new EventSource<long>();\n"
        "  end M;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInvarianceDiagnostic(r));
}

// (4) Mirror of (3) with `SharedEventSource`.
BEZEL_TEST(event_source_variance_diagnostic_4676, sharedeventsource_int_from_long_rejected_with_named_diagnostic) {
    auto r = parseSemaAll(
        "module M;\n"
        "public class Program\n"
        "  public method M()\n"
        "    var dst: SharedEventSource<int> := new SharedEventSource<long>();\n"
        "  end M;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInvarianceDiagnostic(r));
}

// ---- ACCEPT controls (regression guards) ----

// (5) IDENTITY on the source-class heads stays ACCEPTED: `EventSource<int>`
// from `new EventSource<int>()` is identity — the only legal variance
// assignment §12.9 permits on the source classes. Confirms the new diagnostic
// path does not over-reject legitimate source-class identity.
BEZEL_TEST(event_source_variance_diagnostic_4676, eventsource_int_identity_accepted) {
    auto r = parseSemaAll(
        "module M;\n"
        "public class Program\n"
        "  public method M()\n"
        "    using var src: EventSource<int> := new EventSource<int>();\n"
        "  end M;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (6) Mirror of (5) with `SharedEventSource`.
BEZEL_TEST(event_source_variance_diagnostic_4676, sharedeventsource_int_identity_accepted) {
    auto r = parseSemaAll(
        "module M;\n"
        "public class Program\n"
        "  public method M()\n"
        "    using var src: SharedEventSource<int> := new SharedEventSource<int>();\n"
        "  end M;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (7) Scope pin: a non-source-class generic head (`Box<T>`) where the
// covariance fails still gets the GENERIC §5.1 diagnostic, NOT the new
// source-class invariance needle. Pins that the named diagnostic does not
// over-fire on unrelated generic heads.
BEZEL_TEST(event_source_variance_diagnostic_4676, non_source_class_generic_uses_generic_diagnostic) {
    auto r = parseSemaAll(
        "module M;\n"
        "public class Box<T>\n"
        "  public value V: T;\n"
        "  public constructor(v: T) end constructor;\n"
        "end Box;\n"
        "public class Program\n"
        "  public method M()\n"
        "    var dst: Box<int> := new Box<long>(1);\n"
        "  end M;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInvarianceDiagnostic(r));
    BEZEL_EXPECT_TRUE(hasGenericNotConvertible(r));
}
