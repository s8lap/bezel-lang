// Spec-derived regression tests for issue #2050 — the §14.2 "Derived-property
// constraints" admitted-set entry `where T: sendable` (v0.201, proposal 0049
// train B). Satisfaction is decided per instantiation by the §30.14 sendable-
// type computation; the constraint "adds no second definition of the property"
// (§14.2). One diagnostic — the constraint failure at the instantiation site,
// naming the constraint — replaces the interior §3.6 rejection the body would
// otherwise hit.
//
// These tests pin BOTH constraint-check sites:
//   - the method-call site (ExprTypecheck_Calls.cpp's
//     `checkMethodGenericConstraints`), and
//   - the type-instantiation site (Resolver::checkOneConstraint in
//     TypeResolution_Checks.cpp).
//
// Authority: spec §14.2 "Derived-property constraints" + §30.14 sendable-type
// rules (v0.201); stdlib §6.8/§6.9 (the Channel<T> / Domain.Spawn signatures
// carrying the constraint are exercised by the existing #600 / #1490 / #1491
// suites; this file covers the user-facing `where T: sendable` spelling).

#include "support/aot_support.hpp"
#include "support/sema_support.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::SemaResult;

namespace {

// fix #3985: delegates to the shared gated helper (sema_support.hpp)
// instead of duplicating the parse-error-gate logic locally.
bezel::test::SemaResult parseSemaAll(std::string_view src) {
    return bezel::test::parseAndTypecheckGated(src);
}

// True iff a `where T: …` constraint-not-satisfied diagnostic was reported.
bool hasConstraintError(const SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("does not satisfy the constraint") != std::string::npos)
            return true;
    return false;
}

// True iff a constraint-failure diagnostic specifically named `sendable`.
bool hasSendableConstraintError(const SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("does not satisfy the constraint") != std::string::npos &&
            d.message.find("sendable") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ── ACCEPT: sendable substitutions must be UNAFFECTED (no false positives) ──

// §14.2 / §30.14: a primitive value type (`int`) is sendable. The constraint
// is satisfied at the method-call site; the call compiles cleanly.
BEZEL_TEST(sendable_constraint_2050,
           method_call_int_arg_accepted_spec_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "static class Box "
        "  public static method Hold<T>(x: T): int where T: sendable => 1; "
        "end Box; "
        "class C public static method Main(): int "
        "  return Box.Hold(42); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(!hasConstraintError(r));
}

// §14.2 / §30.14: a `sealed` class with only a primitive field is transfer-
// closed, hence sendable. The constraint accepts at the method-call site.
BEZEL_TEST(sendable_constraint_2050,
           method_call_sealed_class_arg_accepted_spec_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "sealed class ClosedData "
        "  public Val: int := 0; "
        "  public constructor(v: int) Val := v; end constructor; "
        "  destructor end destructor; "
        "end ClosedData; "
        "static class Box "
        "  public static method Hold<T>(x: T): int where T: sendable => 1; "
        "end Box; "
        "class C public static method Main(): int "
        "  return Box.Hold(new ClosedData(7)); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(!hasConstraintError(r));
}

// §14.2 / §30.14: a record with only primitive fields is not borrow-bearing,
// hence sendable. The constraint accepts at the method-call site.
BEZEL_TEST(sendable_constraint_2050,
           method_call_record_arg_accepted_spec_30_14) {
    // fix #3985: the source read `record Point(X: int, Y: int); end
    // Point;` — a positional record's `RecordDecl` is already terminated
    // by its own `;` (Appendix A: `PositionalFields? ... (";" |
    // RecordBody)`), so the trailing `end Point;` is a stray top-level
    // fragment and never parsed; the row was reaching sema on
    // parseSemaAll's ungated, error-recovery-truncated AST. VERIFIED
    // against the real `bezelc -c`: dropping `end Point;` parses cleanly.
    auto r = parseSemaAll(
        "module M; "
        "record Point(X: int, Y: int); "
        "static class Box "
        "  public static method Hold<T>(x: T): int where T: sendable => 1; "
        "end Box; "
        "class C public static method Main(): int "
        "  return Box.Hold(new Point(3, 4)); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(!hasConstraintError(r));
}

// §14.2 / §30.14: type-instantiation site — `Mailbox<ClosedData>` against
// `class Mailbox<T> where T: sendable` is accepted (sealed-class arg).
BEZEL_TEST(sendable_constraint_2050,
           type_inst_sealed_class_arg_accepted_spec_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "sealed class ClosedData "
        "  public Val: int := 0; "
        "  public constructor(v: int) Val := v; end constructor; "
        "  destructor end destructor; "
        "end ClosedData; "
        "public class Mailbox<T> where T: sendable "
        "  public constructor() end constructor; "
        "end Mailbox; "
        "class C public static method Main(): int "
        "  using var mb := new Mailbox<ClosedData>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(!hasConstraintError(r));
}

// ── REJECT: non-sendable substitutions must produce the constraint diagnostic ─

// §14.2 / §30.14: `Object` is non-sendable (runtime type behind it is
// unverifiable). The method-call-site constraint check must reject it with
// the §14.2 constraint-failure diagnostic naming `sendable`.
BEZEL_TEST(sendable_constraint_2050,
           method_call_object_arg_rejected_spec_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "static class Box "
        "  public static method Hold<T>(x: T): int where T: sendable => 1; "
        "end Box; "
        "class C public static method Main(): int "
        "  return Box.Hold(new Object()); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasSendableConstraintError(r));
}

// §14.2 / §30.14: an OPEN user class is not transfer-closed (a subclass could
// add origin-domain references), so it is not sendable. The method-call-site
// check rejects at the constraint.
BEZEL_TEST(sendable_constraint_2050,
           method_call_open_class_arg_rejected_spec_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "class OpenData public Val: int := 0; destructor end destructor; "
        "end OpenData; "
        "static class Box "
        "  public static method Hold<T>(x: T): int where T: sendable => 1; "
        "end Box; "
        "class C public static method Main(): int "
        "  return Box.Hold(new OpenData()); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasSendableConstraintError(r));
}

// §14.2 / §30.14: a function-typed value is never sendable (its captures are
// borrows of the origin domain). The method-call-site check rejects a
// function-typed argument.
BEZEL_TEST(sendable_constraint_2050,
           method_call_function_arg_rejected_spec_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "static class Box "
        "  public static method Hold<T>(x: T): int where T: sendable => 1; "
        "end Box; "
        "static class Twice "
        "  public static method Go(): int => 5; "
        "end Twice; "
        "class C public static method Main(): int "
        "  var f: () -> int := Twice.Go; "
        "  return Box.Hold(f); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasSendableConstraintError(r));
}

// §14.2 / §30.14: type-instantiation site — `Mailbox<OpenData>` against
// `class Mailbox<T> where T: sendable` is rejected at the instantiation site.
BEZEL_TEST(sendable_constraint_2050,
           type_inst_open_class_arg_rejected_spec_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "class OpenData public Val: int := 0; destructor end destructor; "
        "end OpenData; "
        "public class Mailbox<T> where T: sendable "
        "  public constructor() end constructor; "
        "end Mailbox; "
        "class C public static method Main(): int "
        "  using var mb := new Mailbox<OpenData>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasSendableConstraintError(r));
}

// ── COMPOSITION: `where T: sendable, class` (§14.2 v0.201 example) ───────────

// §14.2: "Composes with the other forms (`where T: sendable, class` admits
// sendable class types)." A sealed class passes both. (This exercises the
// composition without depending on Domain.Spawn/Channel — pure §14.2.)
BEZEL_TEST(sendable_constraint_2050,
           method_call_sendable_class_compose_accepted_spec_14_2) {
    auto r = parseSemaAll(
        "module M; "
        "sealed class ClosedData "
        "  public Val: int := 0; "
        "  public constructor(v: int) Val := v; end constructor; "
        "  destructor end destructor; "
        "end ClosedData; "
        "static class Box "
        "  public static method Hold<T>(x: T): int "
        "    where T: sendable, class => 1; "
        "end Box; "
        "class C public static method Main(): int "
        "  return Box.Hold(new ClosedData(7)); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(!hasConstraintError(r));
}

// §14.2: a primitive `int` against `where T: sendable, class` fails `class`
// (the kind constraint runs alongside sendable; either failure rejects).
BEZEL_TEST(sendable_constraint_2050,
           method_call_int_vs_sendable_class_compose_rejected_spec_14_2) {
    auto r = parseSemaAll(
        "module M; "
        "static class Box "
        "  public static method Hold<T>(x: T): int "
        "    where T: sendable, class => 1; "
        "end Box; "
        "class C public static method Main(): int "
        "  return Box.Hold(42); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}

// ── NON-REGRESSION: existing Channel<T> rejection still fires ────────────────

// §30.15 / §3.6: `new Channel<OpenClass>()` is rejected at the `new` site by
// the existing §30.14 check (untouched by the constraint work). The diagnostic
// names §30.14/§30.15, NOT §14.2 — the constraint-aware path is the new
// `where T: sendable` spelling; the call-site-arg check stays as before.
BEZEL_TEST(sendable_constraint_2050,
           channel_non_sendable_class_element_still_rejected_30_15) {
    auto r = parseSemaAll(
        "module M; class OpenData public Val: int := 0; "
        "destructor end destructor; end OpenData; "
        "class C public static method Test() "
        "  using var ch := new Channel<OpenData>(); "
        "end Test; end C;");
    bool sawChannelReject = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("is not sendable") != std::string::npos &&
            d.message.find("Channel") != std::string::npos)
            sawChannelReject = true;
    BEZEL_EXPECT_TRUE(sawChannelReject);
}
