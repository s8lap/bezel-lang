// Spec-derived tests for §31.2.1 / §12.7 / §2.8.3 rule 4 / §17.4, §19.3/#1927,
// #1939 (spec v0.298):
//   the catch / case / `is`/`as` erasure guards test "does this type argument
//   mention a free type variable?" by structural shape alone
//   (`kind == Named && declaration == nullptr && genericArgs.empty()` plus a
//   non-empty head name), then ASSUME that positive answer. ~50 null-
//   declaration stdlib builtins (`Task`, `Exception`, `IDisposable`, `Mutex`,
//   `Semaphore`, `DateTime`, `TimeSpan`, `Guid`, `StringBuilder`, `Random`,
//   `File`, `Path`, `IntPtr`, `CString`, ...) match that exact shape — they
//   are NEITHER free type parameters NOR "type variables" at all, just
//   resolved-to-known types. The guards silently treat them as variables and
//   skip the runtime-erasure diagnostic, so an admitted `MyExc<Task>` arm /
//   `Wrap<Task>` `is` test / `var x: Wrap<Task>` case binds a value whose
//   runtime descriptor cannot distinguish per-monomorphization — the
//   §2.8.3 rule 4 confusion class.
//
// The fix: the three structural spellings (`isFreeGenericTypeVarRef`,
// `refMentionsTypeVariable`, `typeTestTargetMentionsTypeVariable`) must
// consult the ENCLOSING SCOPE'S DECLARED TYPE-PARAMETER NAMES —
// `currentScopeGenericParamNames()` mirrored by
// `refOutParamTypeMentionsTypeParam` — not the bare shape. Tests (1)/(2) pin
// the catch-arm path, (3)/(4) the plain `is`/`as` path, (5) the value-form
// `case` path. (6)/(7)/(8) are over-rejection guards: a real generic
// parameter at a generic scope must still be admitted, and a non-builtin
// concrete (reified) type argument must remain accepted.
//
// FAILS WITHOUT the fix on tests (1), (2), (3), (4), (5): the false positive
// in the structural predicate holds the rejection off, so the diagnostic
// never fires.

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

// Catch-arm surface diagnostic — see
// tests/sema/catch_arm_erased_typearg_1939_test.cpp. The phrase
// "not runtime-distinguishable" identifies the §31.2.1 / #1939 guard.
bool hasCatchErasedError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("not runtime-distinguishable") != std::string::npos &&
            d.message.find("this `catch` arm type") != std::string::npos)
            return true;
    }
    return false;
}

// Plain `is`/`as` surface diagnostic — see
// tests/sema/is_as_type_test_erased_2006_test.cpp.
bool hasPlainErasedError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("`is`/`as` target is not runtime-distinguishable") !=
                std::string::npos)
            return true;
    }
    return false;
}

// Value-form `case` expression surface diagnostic — see
// tests/sema/case_type_test_value_form_erased_2018_test.cpp. The phrase
// "runtime type descriptor collapses" identifies the §17.4/§12.7/§2.8.3 rule 4
// guard on the binding-pattern type.
bool hasValueCaseErasedError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("binding pattern type") != std::string::npos &&
            d.message.find("runtime type descriptor collapses") !=
                std::string::npos)
            return true;
    }
    return false;
}

std::string wrap(const std::string& body) {
    return "module M;\n"
           "class MyExc<T>: Exception\n"
           "  public constructor(msg: string) base(msg); end constructor;\n"
           "end MyExc;\n"
           "class Wrap<W>\n"
           "end Wrap;\n"
           "class Program\n" +
           body + "end Program;\n";
}

}  // namespace

// ============================================================================
// CATCH-ARM PATH (#1939 surface, refMentionsTypeVariable in
// visitTryStmt's catch loop).
// ============================================================================

// (1) HEAD false positive: `MyExc<Task>` is silently accepted because the
// structural predicate reads a null-declaration no-args Named as "mentions a
// free variable" and suppresses the §31.2.1 diagnostic. fix #4229 must consult
// `currentScopeGenericParamNames()` — Task is NOT a scope param, the guard
// proceeds to the reification check, which sees Task is null-decl+no-args
// and emits the runtime-distinguishable diagnostic.
BEZEL_TEST(builtin_typearg_erasure_misclass_4229,
           catch_arm_builtin_type_arg_rejected) {
    auto r = parseSemaAll(wrap(
        "  public method M(): int\n"
        "    try\n"
        "      return 1;\n"
        "    catch\n"
        "      var e: MyExc<Task> => do return 2; end\n"
        "    end;\n"
        "    return 0;\n"
        "  end M;\n"));
    BEZEL_EXPECT_TRUE(hasCatchErasedError(r));
}

// (2) Same as (1) but inside a GENERIC scope (`Probe<T>`) where T is a real
// generic parameter — confirming the predicate stays ACTIVE for genuine type
// variables (`var e: MyExc<T>` stays accepted via test (6)) and only switches
// off the false positive for shapes that have no parameter in scope. Task
// remains outside the scope's parameter names → REJECTED.
BEZEL_TEST(builtin_typearg_erasure_misclass_4229,
           catch_arm_builtin_type_arg_generic_scope_rejected) {
    auto r = parseSemaAll(wrap(
        "  public method Probe<T>(action: () -> ()): int\n"
        "    try\n"
        "      action();\n"
        "    catch\n"
        "      var e: MyExc<Task> => do return 1; end\n"
        "    end;\n"
        "    return 0;\n"
        "  end Probe;\n"));
    BEZEL_EXPECT_TRUE(hasCatchErasedError(r));
}

// ============================================================================
// PLAIN `is`/`as` PATH (#2006 surface,
// typeTestTargetMentionsTypeVariable).
// ============================================================================

// (3) Same false-positive class: `o is Wrap<Task>` reaches codegen because
// the structural predicate reads Task as a free variable. The runtime test
// cannot distinguish `Wrap<Task>` from any other `Wrap<X>` whose X has the
// same null-decl+no-args descriptor-key contribution. REJECTED.
BEZEL_TEST(builtin_typearg_erasure_misclass_4229,
           is_as_builtin_type_arg_rejected) {
    auto r = parseSemaAll(wrap(
        "  public method M(o: Object): int\n"
        "    return (if o is Wrap<Task> then 1 else 0);\n"
        "  end M;\n"));
    BEZEL_EXPECT_TRUE(hasPlainErasedError(r));
}

// (4) Same shape under `as` — the follow-on reinterpreting cast is the
// memory-unsafe half of the confusion class.
BEZEL_TEST(builtin_typearg_erasure_misclass_4229,
           as_builtin_type_arg_rejected) {
    auto r = parseSemaAll(wrap(
        "  public method M(o: Object): int\n"
        "    var x := o as Wrap<Task>;\n"
        "    return (if x = null then 1 else 0);\n"
        "  end M;\n"));
    BEZEL_EXPECT_TRUE(hasPlainErasedError(r));
}

// ============================================================================
// VALUE-FORM `case` BINDING-PATTERN PATH (#2018 surface,
// typeTestTargetMentionsTypeVariable in ExprTypecheck_Exprs.cpp's
// visitExpr CaseExpr handler).
// ============================================================================

// (5) A `case o of var x: Wrap<Task> => 1, _ => 0` is the value/expression
// analog of test (3) — the same null-decl+no-args Task argument reaches
// codegen with the same false positive. REJECTED.
BEZEL_TEST(builtin_typearg_erasure_misclass_4229,
           case_value_form_builtin_type_arg_rejected) {
    auto r = parseSemaAll(wrap(
        "  public method M(o: Object): int\n"
        "    var n := (case o of\n"
        "      var x: Wrap<Task> => 1,\n"
        "      _ => 0\n"
        "    );\n"
        "    return n;\n"
        "  end M;\n"));
    BEZEL_EXPECT_TRUE(hasValueCaseErasedError(r));
}

// ============================================================================
// OVER-REJECTION GUARDS — Pin that the fix preserves the existing
// conservative-accept posture on REAL free type parameters, and that
// reified-with-ground-builtin arguments stay admitted.
// ============================================================================

// (6) PIN-POSITIVE: a real free type parameter at the same call site stays
// admitted under the fix. `var e: MyExc<T>` inside `Probe<T>` — T is in the
// scope's `currentScopeGenericParamNames()`, so the predicate returns true
// and the guard suppresses the diagnostic (T resolves to a concrete arg at
// monomorphization, §31.2.1 / #1843). Stays ACCEPTED.
BEZEL_TEST(builtin_typearg_erasure_misclass_4229,
           catch_arm_free_type_param_stays_admitted) {
    auto r = parseSemaAll(wrap(
        "  public method Probe<T>(action: () -> ()): int\n"
        "    try\n"
        "      action();\n"
        "    catch\n"
        "      var e: MyExc<T> => do return 1; end\n"
        "    end;\n"
        "    return 0;\n"
        "  end Probe;\n"));
    BEZEL_EXPECT_FALSE(hasCatchErasedError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (7) PIN-POSITIVE: a reified ground type argument stays admitted. `var e:
// MyExc<int>` — `int` is reified (§2.8.3 rule 4) so the runtime test IS
// distinguishable, and the guard does not fire. Stays ACCEPTED (control: this
// is the existing behavior of tests/sema/catch_arm_erased_typearg_1939_test).
BEZEL_TEST(builtin_typearg_erasure_misclass_4229,
           catch_arm_reified_arg_stays_admitted) {
    auto r = parseSemaAll(wrap(
        "  public method M(): int\n"
        "    try\n"
        "      return 1;\n"
        "    catch\n"
        "      var e: MyExc<int> => do return 2; end\n"
        "    end;\n"
        "    return 0;\n"
        "  end M;\n"));
    BEZEL_EXPECT_FALSE(hasCatchErasedError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (8) PIN-POSITIVE (is/as): `o is Wrap<T>` inside a generic `Chk<T>` stays
// admitted. T IS in scope, so the predicate returns true and the erasure
// guard's "type-variable target" escape hatch fires exactly as it did before
// the fix. The over-rejection guard of #4229.
BEZEL_TEST(builtin_typearg_erasure_misclass_4229,
           is_as_free_type_param_stays_admitted) {
    auto r = parseSemaAll(wrap(
        "  public method Chk<T>(o: Object): int\n"
        "    return (if o is Wrap<T> then 1 else 0);\n"
        "  end Chk;\n"));
    BEZEL_EXPECT_FALSE(hasPlainErasedError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
