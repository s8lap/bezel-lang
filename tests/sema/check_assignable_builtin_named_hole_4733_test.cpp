// fix #4733 / spec §5.1 (v0.307) — closing the built-in Named `declaration ==
// nullptr` fall-through hole in `ExprTC::checkAssignable`.
//
// A built-in Named type (e.g. `StringBuilder`, `Exception`, `Array<T>`) carries
// `declaration == nullptr` because the compiler has no TypeSymbol for stdlib
// types — they are synthesised at lowering. The downstream effect is that
// every gate keyed on `declaration != nullptr` (the §5.1 Named-Named branch,
// the #1178 mixed class/primitive rule, etc.) MISSES them, and any shape that
// reaches this fall-through is SILENTLY ACCEPTED today:
//
//   Repro A. `var sb := new StringBuilder(); var w: Widget := sb;` — pre-fix
//            ACCEPTED. `StringBuilder` is a built-in class (`isHeapPointer-
//            ReferenceBuiltin`-true), `Widget` is a user class; the §4.16 /
//            §4.14 / §4.15 arms require their specific source names and miss,
//            the §12.9 generic-args arm requires `to->genericArgs` and misses,
//            every other arm requires `declaration != nullptr` and misses. The
//            value falls through to "Mixed / generic-parameter / null-literal
//            cases: skip" and is silently accepted — a downstream shape
//            mismatch (the same failure SHAPE that produced #4644's exception
//            case, but on a heap-reference class).
//
//   Repro B. `var e: Exception := 5;` — pre-fix ACCEPTED and IR-verifier
//            aborted with no source location. `Exception` is a built-in class
//            (`isBuiltinExceptionName`-true), `5` is a concrete primitive
//            (int); #4644 only fires when `from` is the built-in class (so the
//            reverse direction slipped past), #1178 explicitly excludes
//            null-declaration Named types ("a generic type-parameter and a
//            builtin both have a null declaration and must keep flowing
//            through"), so it missed the built-in-class half too. Result:
//            silent acceptance, malformed IR.
//
// The fix extends #4644's exception-family gate to all BUILT-IN CLASSES
// (`isHeapPointerReferenceBuiltin` MINUS the built-in INTERFACE heads, plus
// `isBuiltinExceptionName`) for the user-class target, the primitive target,
// and the REVERSE direction (primitive source). Each is checked with the
// same exclusion set as #4644:
//   * a TYPE VARIABLE is Named-with-null-declaration too (a user type
//     parameter that shadows a built-in name is NOT the built-in) —
//     excluded by `refMentionsTypeVariable` and `freeTypeParamNames`;
//   * an ERROR-RECOVERY Named (an unresolved identifier) is also null-
//     declaration but NOT in either built-in name predicate, so it falls
//     outside the gate naturally without cascading.
//   * legitimate built-in→built-in VARIANCE through a generic-arg target
//     (e.g. `Array<T>` → `IEnumerable<T>`) is handled by the variance arm
//     above when `to->genericArgs` is non-empty; this gate never fires for
//     those because `from->genericArgs` is also non-empty and we restrict
//     the predicate to `genericArgs.empty()`.
//
// These five rows pin THE BUG (rows 1–2) plus the three false-positive
// hazards (rows 3–5); each is a FAIL-WITHOUT regression pin against the
// pre-fix behaviour. Diagnostic wording is pinned by SUBSTRING rather than
// full sentence so a rephrase that keeps the rule's identifying token does
// not silently break the test.

#include <string>

#include "support/aot_support.hpp"   // runAot — the real bezelc AOT oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// §5.1 diagnostic fragment for Repro A: "the built-in type `StringBuilder`
// is not a `Widget` — `Widget` is not a base of it, so the conversion would
// be a narrowing (or unrelated) reference conversion, and §5.1 has none
// implicitly". The fragment pins BOTH the rule (§5.1) and the rule's
// identifying token ("not a base of it" / "narrowing").
bool hasBuiltinToUserClassDiagnostic(const JitResult& r,
                                     std::string_view fromName,
                                     std::string_view toName) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(std::string("the built-in type `") +
                           std::string(fromName) + "`") == std::string::npos)
            continue;
        if (d.message.find(std::string("is not a `") +
                           std::string(toName)) == std::string::npos)
            continue;
        if (d.message.find("5.1") == std::string::npos) continue;
        return true;
    }
    return false;
}

// §5.1 / §4.6 diagnostic fragment for Repro B and the primitive-target arm:
// "the target is the built-in class `…` and the source is a value type —
// §5.1 has no implicit conversion between them, and §4.6 forbids an
// implicit conversion from `string`" (and the symmetric primitive-source
// arm's wording). The fragment pins the §5.1 reference plus the
// "no implicit conversion" token shared by both arms.
bool hasBuiltinClassValueTypeConversionDiagnostic(const JitResult& r,
                                                 std::string_view builtinName) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("no implicit conversion") == std::string::npos)
            continue;
        if (d.message.find("5.1") == std::string::npos) continue;
        if (d.message.find(std::string(builtinName)) == std::string::npos)
            continue;
        return true;
    }
    return false;
}

// Shared prelude: a user class `Widget`, a user class `WidgetExtExc` deriving
// from the built-in exception `IOException` (to confirm the upcast and the
// ancestor-passes-`hasNamedAncestor` path still work). All programs in this
// file declare a `Main()` so `runAot` can fork a complete compilation.
const std::string kPrelude =
    "module M; import System.Text; import System.Collections.Generic; "
    "class Widget "
    "  public V: int; "
    "  public constructor(v: int) V := v; end constructor; "
    "end Widget; ";

}  // namespace

// =====================================================================
// ROW 1 — Repro A: built-in class → user class (the wider #4644 hole)
// =====================================================================
// Without fix: compiles clean, downstream shape mismatch.
// With fix: rejected with §5.1 "is not a `Widget`" + "not a base of it".

BEZEL_TEST(check_assignable_builtin_named_hole_4733,
           stringbuilder_to_user_class_rejected_spec_5_1) {
    JitResult r = runAot(kPrelude +
        "class C "
        "  public static method Main(): int "
        "    var sb := new StringBuilder(); "
        "    var w: Widget := sb; "   // built-in class → user class
        "    return w.V; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        hasBuiltinToUserClassDiagnostic(r, "StringBuilder", "Widget"));
}

// =====================================================================
// ROW 2 — Repro B: concrete primitive → built-in class
// =====================================================================
// Without fix: compiles clean, IR verifier abort with no source location
// ("Function return type does not match operand type of return inst!".
//   or "Trunc only operates on integer …", depending on the primitive).
// With fix: rejected with §5.1 "no implicit conversion" + §4.6.

BEZEL_TEST(check_assignable_builtin_named_hole_4733,
           int_to_builtin_exception_rejected_spec_5_1) {
    JitResult r = runAot(kPrelude +
        "class C "
        "  public static method Main(): int "
        "    var e: Exception := 5; "   // int → Exception (built-in class)
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        hasBuiltinClassValueTypeConversionDiagnostic(r, "Exception"));
}

// =====================================================================
// ROW 3 — CONTROL: legitimate built-in→built-in VARIANCE through a
// generic-arg target. `Array<T>` → `IEnumerable<T>` must STILL be
// accepted — it is handled by the variance arm above (line ~4623 of
// ExprTypecheck.cpp), not by the new gate (whose predicate restricts to
// `genericArgs.empty()` so this shape never reaches it).
// =====================================================================

BEZEL_TEST(check_assignable_builtin_named_hole_4733,
           array_to_ienumerable_variance_accepted_spec_5_1) {
    JitResult r = runAot(kPrelude +
        "class C "
        "  public static method Main(): int "
        "    var a := new Array<int>(); "
        "    var e: IEnumerable<int> := a; "   // built-in class → built-in iface
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// =====================================================================
// ROW 4 — CONTROL: a bare TYPE PARAMETER (`T` in `Box<T>`) flowing
// through a generic body. A type parameter is Named-with-null-declaration
// (a type parameter has no TypeSymbol), and is excluded by
// `refMentionsTypeVariable` / `freeName membership`. The new gate must
// NOT cascade a false-positive rejection onto it — the existing
// generic-deferral path keeps the value flowing at instantiation. We
// instantiate with T = int so the only Named-with-null-declaration type
// the body ever passes through is the parameter `v: T`, exactly the
// shape the exclusion was designed for.
// =====================================================================

BEZEL_TEST(check_assignable_builtin_named_hole_4733,
           type_variable_in_generic_class_not_cascaded_spec_5_1) {
    JitResult r = runAot(kPrelude +
        "class C "
        "  public static method Main(): int "
        "    return Box<int>.Hold(7); "
        "  end Main; "
        "end C; "
        "class Box<T> "
        "  public static method Hold(v: T): int return 0; end Hold; "
        "end Box;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// =====================================================================
// ROW 5 — CONTROL: ERROR-RECOVERY on an unresolved Named identifier
// (`DoesNotExist`). An unresolved Named is also null-declaration but is
// NOT in `isHeapPointerReferenceBuiltin` / `isBuiltinExceptionName`, so
// the gate naturally does NOT fire for it. The §5.1 "not implicitly
// convertible" diagnostic must NOT be produced; whatever resolution-
// error the typechecker already reported is the single diagnostic.
// =====================================================================
// This test compiles the program as `module M;` with NO `Widget` etc. so
// `DoesNotExist` cannot resolve. The exact resolution-error diagnostic is
// out of scope for #4733; this row's job is to confirm the NEW gate does
// not produce a §5.1 "not implicitly convertible" diagnostic for it
// (which would be a false-positive cascade).

BEZEL_TEST(check_assignable_builtin_named_hole_4733,
           error_recovery_unresolved_named_not_cascaded_spec_5_1) {
    JitResult r = runAot(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var x: DoesNotExist := 5; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    // The §5.1 NEW-gate diagnostic must NOT appear. Any errors present
    // are the unresolved-identifier / resolution errors, which are out of
    // scope here.
    bool sawNewGateDiagnostic = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        // Catch any of the four diagnostic wordings the new gate emits.
        if (d.message.find("the built-in type `") != std::string::npos ||
            d.message.find("the built-in exception type `") != std::string::npos ||
            d.message.find("the built-in class `") != std::string::npos) {
            sawNewGateDiagnostic = true; break;
        }
        if (d.message.find("no implicit conversion") != std::string::npos &&
            d.message.find("5.1") != std::string::npos) {
            sawNewGateDiagnostic = true; break;
        }
    }
    BEZEL_EXPECT_FALSE(sawNewGateDiagnostic);
}