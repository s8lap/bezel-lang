// Spec-derived regression tests for issue #8163 — §22.15 *Pure members*
// ("Pure constructors" and "The pure body"), §12.8 *Constructor chaining*,
// §22.10 *Primary constructors*, §22.5 *Optional parameters and named
// arguments*, §22.8 *Field initializers* and §31.4 *Standard exception
// types* — language spec v0.355.
//
// THE DEFECT (`src/sema/PureChecker.cpp`, `PureBodyWalker::
// checkPureCtorChainPurity`).
//
// §22.15 "Pure constructors" — "A pure constructor's `base(…)`/`this(…)`
// chain target must itself be pure." The checker resolved the chain target
// to a `ConstructorSymbol` of the target class. A class PRIMARY constructor
// has NO `ConstructorSymbol` — its parameters live on the `ast::TypeDecl`
// (`hasPositionalFields` / `positionalFields`) — so the lookup found
// nothing and the function returned SILENTLY, admitting the program.
//
// §12.8: a `this(...)` chain in a primary-constructor class ENDS at the
// primary (the primary is that class's only constructor that does not chain
// with `this(...)`), and §22.8 step 0-2 / §22.10: the primary's
// construction runs the inheritance clause's base call and then the class's
// field initializers, for EVERY instance of the class. So the pure
// constructor chains to a construction that runs an impure call, and §22.15's
// rule never fired.
//
// THE REPRO (the issue's own shape):
//
//   class MyError(message: string): Exception(Helper.Msg(message))
//     public pure constructor() this("default"); end constructor;
//   end MyError;
//
// `Helper.Msg` is not `pure`-declared, so the primary's base-call argument
// is an impure call in a pure construction. Before the fix the class was
// admitted AND `throw new MyError()` inside a `pure` method was admitted —
// the side effect ran out of a pure body.
//
// THE ORACLE. Two, deliberately.
//
//   * The SEMA oracle (`parseAndCheckPureGated`) is the phase under test: it
//     runs lex + parse + symbols + types + expressions + the §22.15
//     pure-body checker, gated exactly as the driver gates them. Each REJECT
//     row asserts the EXACT diagnostic text AND the LINE the diagnostic lands
//     on (the impure call in the class header), so a row cannot pass by
//     observing the absence of something.
//   * The AOT oracle (`runAot`) is the strict whole-program oracle: the real
//     `Driver::compileFile`, forked, then clang-link and native exec. The
//     REJECT rows assert the driver refuses the program with the same
//     diagnostic; the ADMIT row asserts the program COMPILES, LINKS and RUNS
//     (exit code 0), so "it compiled" is never the whole assertion.
//
// The sources are written with real newlines so each rejected row pins the
// diagnostic to its own line.

#include "sema/ExprTypecheck.hpp"
#include "sema/PureChecker.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/aot_support.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include <string>
#include <vector>

namespace {

// The SEMA oracle — the pure-body checker under test.
bezel::test::SemaResult pureCheck(std::string_view src) {
    return bezel::test::parseAndCheckPureGated(src);
}

// The diagnostics whose message contains `needle`.
std::vector<const bezel::Diagnostic*> diagnosticsContaining(
    const bezel::test::SemaResult& r, const std::string& needle) {
    std::vector<const bezel::Diagnostic*> out;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) out.push_back(&d);
    return out;
}

// The ONE diagnostic whose message contains `needle` (asserts there is
// EXACTLY one, so a row cannot pass on a stray diagnostic that happens to
// carry the same words).
const bezel::Diagnostic* theDiagnosticContaining(
    const bezel::test::SemaResult& r, const std::string& needle) {
    auto hits = diagnosticsContaining(r, needle);
    if (hits.size() != 1) return nullptr;
    return hits.front();
}

// The AOT oracle — the real driver, forked. On a compile reject the driver's
// stderr lands in ONE Error diagnostic, so the needle is matched against that
// text.
bool aotRejectsWith(std::string_view src, const std::string& needle) {
    auto r = bezel::test::runAot(src);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// The AOT oracle's ADMIT side: the program must compile, LINK and RUN, and
// exit 0. `r.ran` false would mean the compile or the link failed, so the
// exit-code assertion alone is not enough.
bool aotRunsWithExitZero(std::string_view src) {
    auto r = bezel::test::runAot(src);
    return r.ran && r.exitCode == 0;
}

// §22.15 rule 6's diagnostic for a USER member that is not `pure` — the one
// this file's rows assert. It is the SAME text the class-level birth-store
// walk already reports for an impure field initializer of a class with a pure
// constructor, because a class primary's construction content is the same
// kind of material (§22.8 step 0-2: the base call and the field initializers
// both run as part of the construction).
constexpr const char* kUserImpureCall =
    "the resolved target `Msg` is not pure-declared";

// The program shape shared by most rows: an impure `Helper.Msg`, a class
// whose PRIMARY constructor's base call runs it, and a `pure` convenience
// constructor chaining `this(...)` to that primary.
//
// Line map (the diagnostic lands on line 8, column 31 — the `Helper.Msg`
// call in the class header):
//    1: module M;
//    2: class Helper
//    3:   public static method Msg(m: string): string
//    4:     Console.WriteLine("side effect");
//    5:     return m;
//    6:   end Msg;
//    7: end Helper;
//    8: class MyError(message: string): Exception(Helper.Msg(message))
//    9:   public pure constructor() this("default"); end constructor;
//   10: end MyError;
constexpr const char* kImpureBaseCallArgProgram =
    "module M;\n"
    "class Helper\n"
    "  public static method Msg(m: string): string\n"
    "    Console.WriteLine(\"side effect\");\n"
    "    return m;\n"
    "  end Msg;\n"
    "end Helper;\n"
    "class MyError(message: string): Exception(Helper.Msg(message))\n"
    "  public pure constructor() this(\"default\"); end constructor;\n"
    "end MyError;\n";

}  // namespace

// =========================================================================
// ROW 1 — the defect itself: a pure constructor whose `this(...)` chain
// targets a class PRIMARY whose base-call argument is an impure call.
// §22.15 "Pure constructors" + §12.8 + §22.10.
// =========================================================================

// §22.15: "A pure constructor's `base(…)`/`this(…)` chain target must itself
// be pure." The chain target here is the class PRIMARY, and the primary's
// base call runs `Helper.Msg`, which is not `pure`-declared — REJECTED. The
// diagnostic names the impure member and lands on the call in the class
// header.
BEZEL_TEST(pure_ctor_chain_primary_8163,
           pure_ctor_chain_to_impure_primary_base_call_arg_rejected_8163) {
    auto r = pureCheck(kImpureBaseCallArgProgram);
    const bezel::Diagnostic* d = theDiagnosticContaining(r, kUserImpureCall);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) BEZEL_EXPECT_TRUE(d->location.line == 8u);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// The AOT arm: the real driver refuses the same program with the same
// diagnostic.
BEZEL_TEST(pure_ctor_chain_primary_8163,
           pure_ctor_chain_to_impure_primary_base_call_arg_rejected_by_driver_8163) {
    BEZEL_EXPECT_TRUE(aotRejectsWith(kImpureBaseCallArgProgram,
                                     kUserImpureCall));
}

// =========================================================================
// ROW 2 — the issue's own row: `throw new MyError()` inside a `pure` method.
// The throw invokes the pure convenience constructor, whose chain reaches the
// impure primary; the program is REJECTED (the impure construction is
// reached through the throw, so the pure body cannot promise effect-freedom).
// =========================================================================

BEZEL_TEST(pure_ctor_chain_primary_8163,
           throw_in_pure_method_through_impure_primary_rejected_8163) {
    auto r = pureCheck(
        "module M;\n"
        "class Helper\n"
        "  public static method Msg(m: string): string\n"
        "    Console.WriteLine(\"side effect\");\n"
        "    return m;\n"
        "  end Msg;\n"
        "end Helper;\n"
        "class MyError(message: string): Exception(Helper.Msg(message))\n"
        "  public pure constructor() this(\"default\"); end constructor;\n"
        "end MyError;\n"
        "class Program\n"
        "  public static pure method Run(x: int): int\n"
        "    if x < 0 then\n"
        "      throw new MyError();\n"
        "    end;\n"
        "    return x;\n"
        "  end Run;\n"
        "  public static method Main(): int\n"
        "    try\n"
        "      Console.WriteLine(Run(-1));\n"
        "    catch var e: Exception => Console.WriteLine(\"caught \" + e.Message);\n"
        "    end;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    const bezel::Diagnostic* d = theDiagnosticContaining(r, kUserImpureCall);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) BEZEL_EXPECT_TRUE(d->location.line == 8u);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// The AOT arm of row 2: the driver refuses the program (it does not compile,
// link and run).
BEZEL_TEST(pure_ctor_chain_primary_8163,
           throw_in_pure_method_through_impure_primary_rejected_by_driver_8163) {
    BEZEL_EXPECT_TRUE(aotRejectsWith(
        "module M;\n"
        "class Helper\n"
        "  public static method Msg(m: string): string\n"
        "    Console.WriteLine(\"side effect\");\n"
        "    return m;\n"
        "  end Msg;\n"
        "end Helper;\n"
        "class MyError(message: string): Exception(Helper.Msg(message))\n"
        "  public pure constructor() this(\"default\"); end constructor;\n"
        "end MyError;\n"
        "class Program\n"
        "  public static pure method Run(x: int): int\n"
        "    if x < 0 then\n"
        "      throw new MyError();\n"
        "    end;\n"
        "    return x;\n"
        "  end Run;\n"
        "  public static method Main(): int\n"
        "    try\n"
        "      Console.WriteLine(Run(-1));\n"
        "    catch var e: Exception => Console.WriteLine(\"caught \" + e.Message);\n"
        "    end;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        kUserImpureCall));
}

// =========================================================================
// ROW 3 — the ADMITTING TWIN (the trap): the SAME shape with a PURE
// primary — a `pure` base-call argument and a pure base constructor. §22.15
// admits it, and the fix must not reject it.
//
// This row is the positive control: it shows the shape is admitted rather
// than rejected for an unrelated reason, so the REJECT rows above are
// rejected because of the impurity and not because a primary-constructor
// class can never hold a pure constructor.
// =========================================================================

BEZEL_TEST(pure_ctor_chain_primary_8163,
           pure_ctor_chain_to_pure_primary_admitted_8163) {
    auto r = pureCheck(
        "module M;\n"
        "class Helper\n"
        "  public static pure method Msg(m: string): string\n"
        "    return m;\n"
        "  end Msg;\n"
        "end Helper;\n"
        "class MyError(message: string): Exception(Helper.Msg(message))\n"
        "  public pure constructor() this(\"default\"); end constructor;\n"
        "end MyError;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticsContaining(r, "not pure").empty());
}

// The AOT arm of row 3: the program COMPILES, LINKS and RUNS (exit 0).
BEZEL_TEST(pure_ctor_chain_primary_8163,
           pure_ctor_chain_to_pure_primary_runs_8163) {
    BEZEL_EXPECT_TRUE(aotRunsWithExitZero(
        "module M;\n"
        "class Helper\n"
        "  public static pure method Msg(m: string): string\n"
        "    return m;\n"
        "  end Msg;\n"
        "end Helper;\n"
        "class MyError(message: string): Exception(Helper.Msg(message))\n"
        "  public pure constructor() this(\"default\"); end constructor;\n"
        "end MyError;\n"
        "class Program\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(\"ok\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n"));
}

// =========================================================================
// ROW 4 — the chain through TWO convenience constructors. §12.8: the chain
// must end, and it ends at the primary — so the impurity is reached through
// the second convenience constructor too.
// =========================================================================

BEZEL_TEST(pure_ctor_chain_primary_8163,
           pure_ctor_chain_through_two_convenience_ctors_rejected_8163) {
    auto r = pureCheck(
        "module M;\n"
        "class Helper\n"
        "  public static method Msg(m: string): string\n"
        "    Console.WriteLine(\"side effect\");\n"
        "    return m;\n"
        "  end Msg;\n"
        "end Helper;\n"
        "class MyError(message: string): Exception(Helper.Msg(message))\n"
        "  public pure constructor() this(\"default\"); end constructor;\n"
        "  public pure constructor(a: int) this(\"default\"); end constructor;\n"
        "end MyError;\n");
    const bezel::Diagnostic* d = theDiagnosticContaining(r, kUserImpureCall);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) BEZEL_EXPECT_TRUE(d->location.line == 8u);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// =========================================================================
// ROW 5 — the primary's PARAMETER DEFAULT expression. §22.5: a default-value
// expression evaluates when the argument is omitted, i.e. inside the
// construction the pure constructor chains to; §22.15: "Default-value
// expressions of a pure member's parameters are checked under the pure-body
// rules."
// =========================================================================

BEZEL_TEST(pure_ctor_chain_primary_8163,
           pure_ctor_chain_to_primary_impure_param_default_rejected_8163) {
    auto r = pureCheck(
        "module M;\n"
        "class Helper\n"
        "  public static method Msg(m: string): string\n"
        "    Console.WriteLine(\"side effect\");\n"
        "    return m;\n"
        "  end Msg;\n"
        "end Helper;\n"
        "class MyError(message: string := Helper.Msg(\"default\")): Exception(message)\n"
        "  public pure constructor() this(\"default\"); end constructor;\n"
        "end MyError;\n");
    const bezel::Diagnostic* d = theDiagnosticContaining(r, kUserImpureCall);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) BEZEL_EXPECT_TRUE(d->location.line == 8u);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// The ADMITTING twin of row 5: a PURE default expression.
BEZEL_TEST(pure_ctor_chain_primary_8163,
           pure_ctor_chain_to_primary_pure_param_default_admitted_8163) {
    auto r = pureCheck(
        "module M;\n"
        "class Helper\n"
        "  public static pure method Msg(m: string): string\n"
        "    return m;\n"
        "  end Msg;\n"
        "end Helper;\n"
        "class MyError(message: string := Helper.Msg(\"default\")): Exception(message)\n"
        "  public pure constructor() this(\"default\"); end constructor;\n"
        "end MyError;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =========================================================================
// ROW 6 — the class's FIELD INITIALIZERS. §22.15 "Pure constructors": "The
// rule extends to the class's field initializers ... a class with a pure
// constructor must have field initializers satisfying the pure-body rules."
// The class-level birth-store walk already enforces this — this row is the
// CONTROL that the shape is rejected, and that the fix does not report it
// twice (the primary's construction content is not walked a second time).
// =========================================================================

BEZEL_TEST(pure_ctor_chain_primary_8163,
           pure_ctor_chain_to_primary_impure_field_initializer_rejected_8163) {
    auto r = pureCheck(
        "module M;\n"
        "class Helper\n"
        "  public static method Msg(m: string): string\n"
        "    Console.WriteLine(\"side effect\");\n"
        "    return m;\n"
        "  end Msg;\n"
        "end Helper;\n"
        "class MyError(message: string): Exception(message)\n"
        "  private _n: string := Helper.Msg(\"x\");\n"
        "  public pure constructor() this(\"default\"); end constructor;\n"
        "end MyError;\n");
    const bezel::Diagnostic* d = theDiagnosticContaining(r, kUserImpureCall);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) BEZEL_EXPECT_TRUE(d->location.line == 9u);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// =========================================================================
// ROW 7 — the class WITHOUT a pure constructor is NOT affected: the same
// impure base-call argument in a class whose constructors are all impure (or
// absent) is legal — §22.15's rule binds a PURE constructor's chain target,
// and a class with no pure constructor promises nothing. The fix must not
// turn every primary-constructor class into a rejected one.
// =========================================================================

BEZEL_TEST(pure_ctor_chain_primary_8163,
           impure_class_with_impure_primary_base_call_arg_admitted_8163) {
    auto r = pureCheck(
        "module M;\n"
        "class Helper\n"
        "  public static method Msg(m: string): string\n"
        "    Console.WriteLine(\"side effect\");\n"
        "    return m;\n"
        "  end Msg;\n"
        "end Helper;\n"
        "class MyError(message: string): Exception(Helper.Msg(message))\n"
        "  public constructor() this(\"default\"); end constructor;\n"
        "end MyError;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =========================================================================
// narrow #8163 (round 3) — the primary's invoked BASE-CONSTRUCTOR target.
// §22.15 (v0.355): "A pure constructor's `base(...)`/`this(...)` chain target
// must itself be pure." The primary `class E(m): ImpureBase(m)` invokes
// ImpureBase's body constructor, which a pure constructor chained to E's
// primary therefore runs. The diagnostic is the chain-call family's text.
// =========================================================================

namespace {
constexpr const char* kBaseChainNeedle =
    "base(...) target is not pure";

constexpr const char* kImpureBaseHead =
    "module M;\n"
    "class ImpureBase : Exception\n"
    "  public constructor(m: string) base(m);\n"
    "    Console.WriteLine(\"impure base ctor\");\n"
    "  end constructor;\n"
    "end ImpureBase;\n";
constexpr const char* kPureBaseHead =
    "module M;\n"
    "class ImpureBase : Exception\n"
    "  public pure constructor(m: string) base(m);\n"
    "  end constructor;\n"
    "end ImpureBase;\n";
constexpr const char* kPrimaryE =
    "class E(m: string): ImpureBase(m)\n"
    "  public pure constructor() this(\"d\"); end constructor;\n"
    "end E;\n";
}  // namespace

BEZEL_TEST(pure_ctor_chain_primary_8163,
           pure_ctor_chain_to_primary_with_impure_base_ctor_rejected_8163) {
    auto r = pureCheck(std::string(kImpureBaseHead) + kPrimaryE);
    BEZEL_EXPECT_TRUE(theDiagnosticContaining(r, kBaseChainNeedle) != nullptr);
}

BEZEL_TEST(pure_ctor_chain_primary_8163,
           pure_ctor_chain_to_primary_through_base_primary_rejected_8163) {
    auto r = pureCheck(std::string(kImpureBaseHead) +
        "class B(m: string): ImpureBase(m)\nend B;\n"
        "class E(m: string): B(m)\n"
        "  public pure constructor() this(\"d\"); end constructor;\n"
        "end E;\n");
    BEZEL_EXPECT_TRUE(theDiagnosticContaining(r, kBaseChainNeedle) != nullptr);
}

BEZEL_TEST(pure_ctor_chain_primary_8163,
           pure_ctor_chain_to_primary_base_ctor_arg_impure_rejected_8163) {
    auto r = pureCheck(std::string(kPureBaseHead) +
        "class Helper\n"
        "  public static method Msg(m: string): string\n"
        "    Console.WriteLine(\"x\");\n"
        "    return m;\n"
        "  end Msg;\n"
        "end Helper;\n"
        "class E(m: string): ImpureBase(Helper.Msg(m))\n"
        "  public pure constructor() this(\"d\"); end constructor;\n"
        "end E;\n");
    BEZEL_EXPECT_TRUE(theDiagnosticContaining(r, kUserImpureCall) != nullptr);
}

BEZEL_TEST(pure_ctor_chain_primary_8163,
           pure_ctor_chain_to_primary_with_pure_base_ctor_admitted_8163) {
    auto r = pureCheck(std::string(kPureBaseHead) + kPrimaryE);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(pure_ctor_chain_primary_8163,
           non_pure_ctor_chain_to_primary_with_impure_base_ctor_admitted_8163) {
    auto r = pureCheck(std::string(kImpureBaseHead) +
        "class E(m: string): ImpureBase(m)\n"
        "  public constructor() this(\"d\"); end constructor;\n"
        "end E;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(pure_ctor_chain_primary_8163,
           pure_ctor_chain_to_primary_with_impure_base_ctor_rejected_by_driver_8163) {
    BEZEL_EXPECT_TRUE(aotRejectsWith(
        std::string(kImpureBaseHead) + kPrimaryE +
        "class Program\n"
        "  public static pure method Run(x: int): int\n"
        "    if x < 0 then throw new E(); end;\n"
        "    return x;\n"
        "  end Run;\n"
        "  public static method Main(): int\n"
        "    try Console.WriteLine(Run(-1));\n"
        "    catch var e: Exception => Console.WriteLine(\"caught\");\n"
        "    end;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n", kBaseChainNeedle));
}

BEZEL_TEST(pure_ctor_chain_primary_8163,
           non_pure_ctor_chain_to_primary_with_impure_base_ctor_runs_8163) {
    BEZEL_EXPECT_TRUE(aotRunsWithExitZero(
        std::string(kImpureBaseHead) +
        "class E(m: string): ImpureBase(m)\n"
        "  public constructor() this(\"d\"); end constructor;\n"
        "end E;\n"
        "class Program\n"
        "  public static method Main(): int\n"
        "    var e: E := new E();\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n"));
}
