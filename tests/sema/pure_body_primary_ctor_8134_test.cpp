// Spec-derived regression tests for issue #8134 — §22.15 *Pure members* ("The
// throw carve-out"), §22.10 *Primary constructors*, §22.5 *Optional parameters
// and named arguments*, §22.6 *Overload resolution*, §11.1 *Class declaration*
// (language spec v0.355).
//
// THE DEFECT. §22.15's throw carve-out admits `new E(args)` "only when **(a)**
// the invoked constructor of `E` is `pure`-declared — covering, transitively,
// its chained constructors and `E`'s field initializers — and **(b)** `E` is a
// trivial-dispose class". The carve-out's own fallback (the one the typecheck
// pass does not answer for it) identified the invoked constructor by a rule
// that is FALSE for a class with a PRIMARY constructor:
//
//   class MyError(message: string): Exception(message)
//     public pure constructor() this("default"); end constructor;
//   end MyError;
//
// A primary constructor — the parameter list in the class header (§22.10) — is
// a member of the constructor overload set (§11.1: body constructors are
// declared "in addition" to it) yet carries no `ConstructorSymbol`: its
// parameters live on the `TypeDecl` (`positionalFields` /
// `hasPositionalFields`). So the old rule — "ONE non-static constructor: it is
// the invoked one" — named the PURE convenience constructor for a call the
// PRIMARY actually serves:
//
//   throw new MyError("neg")   // the PRIMARY runs (one argument, one formal)
//
// The carve-out admitted that construction (a SILENT ACCEPTANCE, the review
// r39-rev-8134's NOTE A), while the constructor that runs — a primary — can
// never be `pure`-declared (§22.15 "Where `pure` may appear": a primary is not
// a declaration that carries modifiers; "Members classified by rule" reaches
// only auto-property getters and a record's synthesized members).
//
// THE FIX (this commit). The primary is ONE CANDIDATE AMONG the body
// constructors, never a short-circuit. The candidate set is the primary plus
// the body constructors that BIND (§22.5), and §22.6's ranking — the
// compiler's OWN, with its APPLICABILITY stage ("a candidate whose parameter
// type admits no conversion is [inapplicable]") — decides which of them the
// call invokes. The typecheck pass publishes that winner for the carve-out
// (`SemaContext::pureNewCtor` / `pureNewCtorIsPrimary` /
// `pureNewCtorAmbiguous`), so the carve-out never re-derives it: a SECOND
// ranking, built in the checker, is what the review r40-rev-8134's FINDING 1
// measured — the checker's copy had no APPLICABILITY stage, so it could not
// tell "inapplicable" from "converting" and refused seven legal programs the
// compiler admitted before. The primary is reported with its OWN reason when
// it is the invoked constructor; a set §22.6 cannot separate is refused with
// the GENERIC "could not be identified" reason (never a false statement).
//
// A REJECTED EARLIER ATTEMPT is what the rows below are shaped against: the
// review r40-rev-8133's FINDING 1 measured that the previous lane's commit
// (`a6fa3da8`, rejected) treated the primary as a short-circuit on an
// ARITY-ONLY bind, so a legal `throw new E1(7)` whose invoked constructor is
// the `pure` BODY constructor `constructor(code: int)` was refused with a
// diagnostic whose claim was FALSE (the primary takes a `string`; the
// argument is an `int`). THE ADMITTING TWINS BELOW ARE THE ROWS THAT SEE IT:
// a primary and a body constructor of the SAME arity and a DIFFERENT
// parameter type — an admitting twin the previous attempt's rows could not
// see, because they used `new MyError()` (arity 0 against a 1-parameter
// primary).
//
// THE ORACLE. Two, deliberately.
//
//   * The SEMA oracle (`parseAndCheckPureGated`) is the phase under test: it
//     runs lex + parse + symbols + types + expressions + the §22.15
//     pure-body checker, gated exactly as the driver gates them. Each REJECT
//     row asserts the EXACT diagnostic text and the LINE the diagnostic lands
//     on, so a row cannot pass by observing the absence of something; each
//     ADMIT row asserts that NO error was reported.
//   * The AOT oracle (`runAot` / `runAotOut` / `runAotModules`) is the strict
//     whole-program oracle: the real `Driver::compileFile`, forked, then
//     clang-link and native exec. The ADMIT rows assert that the program
//     COMPILES, LINKS and RUNS — and the rows whose point is the CONSTRUCTOR
//     THAT RUNS assert the printed MESSAGE, so "it compiled" is never the
//     whole assertion. The REJECT rows assert that the driver refuses the
//     program with the same diagnostic.
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

// The ONE diagnostic whose message contains `needle` (asserts there is
// EXACTLY one, so a row cannot pass on a stray diagnostic that happens to
// carry the same words).
const bezel::Diagnostic* theDiagnosticContaining(
    const bezel::test::SemaResult& r, const std::string& needle) {
    std::size_t hits = 0;
    const bezel::Diagnostic* found = nullptr;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) {
            ++hits;
            found = &d;
        }
    if (hits != 1) return nullptr;
    return found;
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

// The captured program stdout with its trailing newline removed — the same
// normalisation `runAotOut` applies to the single-module oracle. The
// multi-module oracle hands back the raw capture (`JitResult::stdoutText`).
std::string trimmedStdout(const std::string& s) {
    std::string out = s;
    if (!out.empty() && out.back() == '\n') {
        out.pop_back();
        if (!out.empty() && out.back() == '\r') out.pop_back();  // CRLF
    }
    return out;
}

// §22.15's throw carve-out's own diagnostic.
constexpr const char* kCarveOut =
    "class construction at `throw` is not admitted in a pure body";
// §22.15's carve-out's PRIMARY reason: the invoked constructor is the class's
// primary constructor, which is never `pure`-declared.
constexpr const char* kPrimaryInvoked =
    "is the class's PRIMARY constructor (\xC2\xA7"
    "22.10), which is never `pure`-declared";
// §22.15's carve-out's reason for an invoked constructor that is not
// `pure`-declared.
constexpr const char* kNotPure = "is not `pure`-declared";
// §22.15's carve-out's GENERIC reason: the invoked constructor could not be
// identified — which is TRUE only where §22.6's ranking genuinely cannot
// separate two APPLICABLE candidates.
constexpr const char* kGenericReason = "could not be identified";
// §22.15 rule 6's diagnostic for a USER member that is not `pure`.
constexpr const char* kUserImpureCall =
    "the resolved target `Count` is not pure-declared";

}  // namespace

// =========================================================================
// THE ADMITTING TWINS — the primary is ONE candidate among the body
// constructors (§22.15 / §22.10 / §22.6).
// =========================================================================

// The review r40-rev-8133's FINDING 1, verbatim in shape: the class has a
// primary `(message: string)` AND a `pure` body constructor `(code: int)` —
// the SAME arity, a DIFFERENT parameter type. `new E1(7)` selects the `pure`
// BODY constructor (the argument is an `int`; the primary takes a `string`),
// so §22.15's carve-out condition (a) holds and the construction is
// ADMITTED.
//
// The previous lane's commit (`a6fa3da8`, rejected) REFUSED this program with
// "the invoked constructor of `E1` is the class's PRIMARY constructor
// (§22.10)" — a false statement: the primary takes a `string` and the
// argument is an `int`.
BEZEL_TEST(pure_body_primary_ctor_8134,
           primary_plus_pure_body_ctor_other_type_throw_admitted_spec_22_15) {
    auto r = pureCheck(
        "module M;\n"
        "class E1(message: string): Exception(message)\n"
        "  public pure constructor(code: int) this(\"code\"); end constructor;\n"
        "end E1;\n"
        "class C\n"
        "  public static pure method B(x: int): int\n"
        "    if x < 0 then\n"
        "      throw new E1(7);\n"
        "    end;\n"
        "    return x;\n"
        "  end B;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The AOT arm of the same shape: the program COMPILES, LINKS and RUNS, and
// the caught exception's MESSAGE is the one the `pure` body constructor
// chained — "code" — which is what proves that the `pure` BODY constructor
// (not the primary) is the constructor that ran.
BEZEL_TEST(pure_body_primary_ctor_8134,
           primary_plus_pure_body_ctor_other_type_throw_runs_spec_22_15) {
    const std::string out = bezel::test::runAotOut(
        "module M;\n"
        "class E1(message: string): Exception(message)\n"
        "  public pure constructor(code: int) this(\"code\"); end constructor;\n"
        "end E1;\n"
        "class C\n"
        "  public static pure method B(x: int): int\n"
        "    if x < 0 then\n"
        "      throw new E1(7);\n"
        "    end;\n"
        "    return x;\n"
        "  end B;\n"
        "end C;\n"
        "class Program\n"
        "  public static method Main(): int\n"
        "    try\n"
        "      Console.WriteLine(C.B(-1));\n"
        "    catch\n"
        "      var e: E1 => Console.WriteLine(e.Message)\n"
        "    end;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_EQ(out, "code");
}

// The same shape with a SECOND `pure` body constructor (`constructor()`): the
// candidate set is {`constructor()` , `constructor(code: int)`, the primary};
// the invoked one is still the `pure` `(code: int)` — the zero-argument
// constructor does not bind the one argument, and the primary binds by arity
// but not by type.
BEZEL_TEST(pure_body_primary_ctor_8134,
           primary_plus_two_pure_body_ctors_throw_admitted_spec_22_15) {
    auto r = pureCheck(
        "module M;\n"
        "class E1(message: string): Exception(message)\n"
        "  public pure constructor() this(\"default\"); end constructor;\n"
        "  public pure constructor(code: int) this(\"code\"); end constructor;\n"
        "end E1;\n"
        "class C\n"
        "  public static pure method B(x: int): int\n"
        "    if x < 0 then\n"
        "      throw new E1(7);\n"
        "    end;\n"
        "    return x;\n"
        "  end B;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The AOT arm of that shape: it runs, and the message is "code" — the
// `(code: int)` constructor's chain, not the zero-argument one's "default".
BEZEL_TEST(pure_body_primary_ctor_8134,
           primary_plus_two_pure_body_ctors_throw_runs_spec_22_15) {
    const std::string out = bezel::test::runAotOut(
        "module M;\n"
        "class E1(message: string): Exception(message)\n"
        "  public pure constructor() this(\"default\"); end constructor;\n"
        "  public pure constructor(code: int) this(\"code\"); end constructor;\n"
        "end E1;\n"
        "class C\n"
        "  public static pure method B(x: int): int\n"
        "    if x < 0 then\n"
        "      throw new E1(7);\n"
        "    end;\n"
        "    return x;\n"
        "  end B;\n"
        "end C;\n"
        "class Program\n"
        "  public static method Main(): int\n"
        "    try\n"
        "      Console.WriteLine(C.B(-1));\n"
        "    catch\n"
        "      var e: E1 => Console.WriteLine(e.Message)\n"
        "    end;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_EQ(out, "code");
}

// The MIRROR of the admitting twin: the primary takes an `int` and the
// `pure` body constructor a `string`, so `new E1(7)` selects the PRIMARY —
// which is never `pure`-declared, and the carve-out must refuse it. (The
// program also carries a §5.1 diagnostic of its own — the `this("m")` chain
// into an `int` primary — which is a DIFFERENT defect and is not this row's
// subject; the row asserts the carve-out's own diagnostic.)
BEZEL_TEST(pure_body_primary_ctor_8134,
           primary_int_plus_pure_string_body_throw_rejected_spec_22_15) {
    auto r = pureCheck(
        "module M;\n"
        "class E1(code: int): Exception(\"e\")\n"
        "  public pure constructor(message: string) this(\"m\"); end "
        "constructor;\n"
        "end E1;\n"
        "class C\n"
        "  public static pure method B(x: int): int\n"
        "    if x < 0 then\n"
        "      throw new E1(7);\n"
        "    end;\n"
        "    return x;\n"
        "  end B;\n"
        "end C;\n");
    const bezel::Diagnostic* d = theDiagnosticContaining(r, kPrimaryInvoked);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) BEZEL_EXPECT_EQ(d->location.line, 8);
}

// =========================================================================
// THE PRIMARY REALLY INVOKED — the under-rejection this commit closes (the
// review r39-rev-8134's NOTE A).
// =========================================================================

// The primary is the ONLY constructor of the class, so the invoked
// constructor IS the primary — and a primary is never `pure`-declared. The
// carve-out refuses, naming the primary.
BEZEL_TEST(pure_body_primary_ctor_8134,
           primary_only_class_throw_rejected_spec_22_15) {
    auto r = pureCheck(
        "module M;\n"
        "class E1(message: string): Exception(message)\n"
        "end E1;\n"
        "class C\n"
        "  public static pure method B(x: int): int\n"
        "    if x < 0 then\n"
        "      throw new E1(\"neg\");\n"
        "    end;\n"
        "    return x;\n"
        "  end B;\n"
        "end C;\n");
    const bezel::Diagnostic* d = theDiagnosticContaining(r, kPrimaryInvoked);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) BEZEL_EXPECT_EQ(d->location.line, 7);
}

// The class has a primary AND a `pure` convenience constructor, and the call
// binds the PRIMARY (one argument against the primary's one formal; the
// convenience constructor takes none). The constructor that runs is the
// primary, so the carve-out refuses — this is the row the previous lane's
// admitting twins could not see (they used the zero-argument call, which
// binds the convenience constructor and IS admitted).
BEZEL_TEST(pure_body_primary_ctor_8134,
           primary_constructor_throw_carve_out_rejected_spec_22_15) {
    auto r = pureCheck(
        "module M;\n"
        "class E1(message: string): Exception(message)\n"
        "  public pure constructor() this(\"default\"); end constructor;\n"
        "end E1;\n"
        "class C\n"
        "  public static pure method B(x: int): int\n"
        "    if x < 0 then\n"
        "      throw new E1(\"neg\");\n"
        "    end;\n"
        "    return x;\n"
        "  end B;\n"
        "end C;\n");
    const bezel::Diagnostic* d = theDiagnosticContaining(r, kPrimaryInvoked);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) BEZEL_EXPECT_EQ(d->location.line, 8);
}

// The AOT arm: the real driver refuses the program with the same diagnostic.
BEZEL_TEST(pure_body_primary_ctor_8134,
           primary_constructor_throw_rejected_by_driver_spec_22_15) {
    BEZEL_EXPECT_TRUE(aotRejectsWith(
        "module M;\n"
        "class E1(message: string): Exception(message)\n"
        "  public pure constructor() this(\"default\"); end constructor;\n"
        "end E1;\n"
        "class C\n"
        "  public static pure method B(x: int): int\n"
        "    if x < 0 then\n"
        "      throw new E1(\"neg\");\n"
        "    end;\n"
        "    return x;\n"
        "  end B;\n"
        "end C;\n",
        kPrimaryInvoked));
}

// =========================================================================
// THE ADMITTING TWINS THAT MUST STAY ADMITTED (the controls).
// =========================================================================

// The zero-argument call against the same class: the `pure` convenience
// constructor IS the invoked one (the primary's one formal is not bound), so
// the carve-out admits it.
BEZEL_TEST(pure_body_primary_ctor_8134,
           primary_plus_pure_convenience_ctor_zero_arg_throw_admitted_spec_22_15) {
    auto r = pureCheck(
        "module M;\n"
        "class E1(message: string): Exception(message)\n"
        "  public pure constructor() this(\"default\"); end constructor;\n"
        "end E1;\n"
        "class C\n"
        "  public static pure method B(x: int): int\n"
        "    if x < 0 then\n"
        "      throw new E1();\n"
        "    end;\n"
        "    return x;\n"
        "  end B;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A class with NO primary at all and one `pure` body constructor: the
// invoked constructor is that constructor.
BEZEL_TEST(pure_body_primary_ctor_8134,
           no_primary_pure_ctor_throw_admitted_spec_22_15) {
    auto r = pureCheck(
        "module M;\n"
        "class E1: Exception\n"
        "  public pure constructor(m: string) base(m); end constructor;\n"
        "end E1;\n"
        "class C\n"
        "  public static pure method B(x: int): int\n"
        "    if x < 0 then\n"
        "      throw new E1(\"neg\");\n"
        "    end;\n"
        "    return x;\n"
        "  end B;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A primary whose formal DECLARES A DEFAULT plus a zero-argument `pure`
// body constructor: the zero-argument call binds both (§22.5), and §22.6's
// defaulted-arity tie-break prefers the candidate that fills no default — the
// `pure` body constructor. Admitted.
BEZEL_TEST(pure_body_primary_ctor_8134,
           primary_default_plus_pure_body_ctor_zero_arg_throw_admitted_spec_22_15) {
    auto r = pureCheck(
        "module M;\n"
        "class E1(message: string := \"default\"): Exception(message)\n"
        "  public pure constructor() this(\"default\"); end constructor;\n"
        "end E1;\n"
        "class C\n"
        "  public static pure method B(x: int): int\n"
        "    if x < 0 then\n"
        "      throw new E1();\n"
        "    end;\n"
        "    return x;\n"
        "  end B;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The invoked constructor is a body constructor that is NOT `pure`: the
// carve-out refuses it with its own reason (not the primary's).
BEZEL_TEST(pure_body_primary_ctor_8134,
           primary_plus_impure_body_ctor_throw_rejected_spec_22_15) {
    auto r = pureCheck(
        "module M;\n"
        "class E1(message: string): Exception(message)\n"
        "  public constructor(code: int) this(\"code\"); end constructor;\n"
        "end E1;\n"
        "class C\n"
        "  public static pure method B(x: int): int\n"
        "    if x < 0 then\n"
        "      throw new E1(7);\n"
        "    end;\n"
        "    return x;\n"
        "  end B;\n"
        "end C;\n");
    const bezel::Diagnostic* d = theDiagnosticContaining(r, kNotPure);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) BEZEL_EXPECT_EQ(d->location.line, 8);
}

// =========================================================================
// THE IMPORTED TWIN — the same decision across the module boundary.
// =========================================================================

// The library module exports a class with a primary `(message: string)` and
// a `pure` body constructor `(code: int)`; the importing module throws it in
// a pure body. The importer's constructor set is read off the artifact (the
// positional parameters are reconstructed from it, #2516), so the decision is
// the same as the local one: the `pure` BODY constructor is the invoked one
// and the carve-out admits the construction.
//
// The oracle is `runAotModules` — the strict multi-module AOT oracle (the
// real driver, a library artifact built with `compileObject`, clang-link,
// native exec). The program must COMPILE, LINK and RUN, and the caught
// message is asserted to be "code" — the `pure` body constructor's chain —
// which is what proves that the `pure` BODY constructor (not the primary) is
// the constructor that ran.
BEZEL_TEST(pure_body_primary_ctor_8134,
           imported_primary_plus_pure_body_ctor_throw_runs_spec_22_15) {
    const std::string lib =
        "module Lib;\n"
        "public class E1(message: string): Exception(message)\n"
        "  public pure constructor(code: int) this(\"code\"); end constructor;\n"
        "end E1;\n";
    const std::string app =
        "module App;\n"
        "import Lib;\n"
        "class C\n"
        "  public static pure method B(x: int): int\n"
        "    if x < 0 then\n"
        "      throw new E1(7);\n"
        "    end;\n"
        "    return x;\n"
        "  end B;\n"
        "end C;\n"
        "class Program\n"
        "  public static method Main(): int\n"
        "    try\n"
        "      Console.WriteLine(C.B(-1));\n"
        "    catch\n"
        "      var e: E1 => Console.WriteLine(e.Message)\n"
        "    end;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
    auto r = bezel::test::runAotModules(
        {bezel::test::ModuleSource{"Lib", lib},
         bezel::test::ModuleSource{"App", app}},
        "App");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
    // The caught message — "code", the `pure` body constructor's chain — so
    // the row proves WHICH constructor ran, not merely that the program ran.
    BEZEL_EXPECT_EQ(trimmedStdout(r.stdoutText), "code");
}

// =========================================================================
// THE RANGE CONSTRUCTION'S OPERANDS (§22.15 rule 3 / rule 6).
// =========================================================================
//
// A range construction is one of the pure body's admitted FORMS (§22.15
// rule 3) — but the CONSTRUCTION, not its operands: the bounds are ordinary
// sub-expressions of the body, and the list of forms is closed ("A pure body
// may contain exactly the following"), so rule 6 governs every call in them
// ("A call — method, operator, accessor read, or constructor in the admitted
// positions — is permitted iff the member … is `pure`-declared or
// pure-classified").
//
// `ast::RangeExpr` had no arm in the walker's expression walk, so
// `0..<Helper.Count()` (a `for` loop's source) and `var r := 1..Helper.Count();`
// (a local's initializer) both compiled in a `pure` body and ran the impure
// call — while the very same call written as `var n := Helper.Count();` was
// rejected.

// The `for` loop's source is a range whose bound calls an impure member.
BEZEL_TEST(pure_body_primary_ctor_8134,
           range_for_source_impure_operand_rejected_spec_22_15) {
    auto r = pureCheck(
        "module M;\n"
        "class H\n"
        "  public static method Count(): int\n"
        "    Console.WriteLine(\"side\");\n"
        "    return 3;\n"
        "  end Count;\n"
        "end H;\n"
        "class C\n"
        "  public static pure method B(): int\n"
        "    var s := 0;\n"
        "    for var i in 0..<H.Count() do\n"
        "      s := s + i;\n"
        "    end;\n"
        "    return s;\n"
        "  end B;\n"
        "end C;\n");
    const bezel::Diagnostic* d = theDiagnosticContaining(r, kUserImpureCall);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) BEZEL_EXPECT_EQ(d->location.line, 11);
}

// The same impurity in a local's initializer (the range is the initializer's
// value).
BEZEL_TEST(pure_body_primary_ctor_8134,
           range_local_initializer_impure_operand_rejected_spec_22_15) {
    auto r = pureCheck(
        "module M;\n"
        "class H\n"
        "  public static method Count(): int\n"
        "    Console.WriteLine(\"side\");\n"
        "    return 3;\n"
        "  end Count;\n"
        "end H;\n"
        "class C\n"
        "  public static pure method B(): int\n"
        "    var r := 1..H.Count();\n"
        "    return 0;\n"
        "  end B;\n"
        "end C;\n");
    const bezel::Diagnostic* d = theDiagnosticContaining(r, kUserImpureCall);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) BEZEL_EXPECT_EQ(d->location.line, 10);
}

// The ADMITTING twin: pure bounds stay admitted.
BEZEL_TEST(pure_body_primary_ctor_8134,
           range_pure_operands_admitted_spec_22_15) {
    auto r = pureCheck(
        "module M;\n"
        "class C\n"
        "  public static pure method B(n: int): int\n"
        "    var s := 0;\n"
        "    for var i in 1..10 do\n"
        "      s := s + i;\n"
        "    end;\n"
        "    var r := 0..<n;\n"
        "    for var k in r do\n"
        "      s := s + k;\n"
        "    end;\n"
        "    return s;\n"
        "  end B;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The AOT arm: the real driver refuses the program with the same
// diagnostic.
BEZEL_TEST(pure_body_primary_ctor_8134,
           range_impure_operand_rejected_by_driver_spec_22_15) {
    BEZEL_EXPECT_TRUE(aotRejectsWith(
        "module M;\n"
        "class H\n"
        "  public static method Count(): int\n"
        "    Console.WriteLine(\"side\");\n"
        "    return 3;\n"
        "  end Count;\n"
        "end H;\n"
        "class C\n"
        "  public static pure method B(): int\n"
        "    var s := 0;\n"
        "    for var i in 0..<H.Count() do\n"
        "      s := s + i;\n"
        "    end;\n"
        "    return s;\n"
        "  end B;\n"
        "end C;\n",
        kUserImpureCall));
}

// =========================================================================
// THE CONVERTING ARGUMENT — §22.6's APPLICABILITY stage (the review
// r40-rev-8134's FINDING 1).
// =========================================================================
//
// §22.6's resolution pipeline, stage 3: "Applicability over the (substituted)
// signatures: parameter types under the implicit conversions (§5.1) and the
// `ref`/`out` modifiers", and §22.6's rule: "a candidate whose parameter type
// admits no conversion is [inapplicable]". So an `int` argument against the
// primary's `string` formal makes the PRIMARY INAPPLICABLE — it drops out at
// stage 3 — and the `pure` body constructor is the only APPLICABLE candidate,
// so it is the invoked one and §22.15's carve-out condition (a) holds.
//
// THE DEFECT THESE ROWS PIN. A checker-side ranking with no APPLICABILITY
// stage cannot tell "inapplicable" from "converting", so it refused these
// programs — legal programs the compiler admitted before the fix.

// A primary `(message: string)` and a `pure` body constructor `(code: long)`,
// called with an `int` ARGUMENT: the `int` converts to the `long` formal
// (§5.1's numeric widening), and no §5.1 conversion turns it into the
// primary's `string` — so the primary is INAPPLICABLE, the `pure` body
// constructor is the invoked one, and the carve-out admits the construction.
BEZEL_TEST(pure_body_primary_ctor_8134,
           primary_plus_pure_body_ctor_widening_arg_throw_admitted_spec_22_6) {
    auto r = pureCheck(
        "module M;\n"
        "class E1(message: string): Exception(message)\n"
        "  public pure constructor(code: long) this(\"code\"); end constructor;\n"
        "end E1;\n"
        "class C\n"
        "  public static pure method B(x: int): int\n"
        "    if x < 0 then\n"
        "      throw new E1(x);\n"
        "    end;\n"
        "    return x;\n"
        "  end B;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The AOT arm of that shape: the program COMPILES, LINKS and RUNS, and the
// caught message is "code" — the `pure` body constructor's chain, so the
// constructor that RAN is the `pure` body constructor (the primary takes a
// `string`, and the argument is an `int`).
BEZEL_TEST(pure_body_primary_ctor_8134,
           primary_plus_pure_body_ctor_widening_arg_throw_runs_spec_22_6) {
    const std::string out = bezel::test::runAotOut(
        "module M;\n"
        "class E1(message: string): Exception(message)\n"
        "  public pure constructor(code: long) this(\"code\"); end constructor;\n"
        "end E1;\n"
        "class C\n"
        "  public static pure method B(x: int): int\n"
        "    if x < 0 then\n"
        "      throw new E1(x);\n"
        "    end;\n"
        "    return x;\n"
        "  end B;\n"
        "end C;\n"
        "class Program\n"
        "  public static method Main(): int\n"
        "    try\n"
        "      Console.WriteLine(C.B(-1));\n"
        "    catch\n"
        "      var e: E1 => Console.WriteLine(e.Message)\n"
        "    end;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_EQ(out, "code");
}

// A converting-to-NULLABLE argument: the primary `(message: string)` and a
// `pure` body constructor `(code: int?)`, called with an `int` ARGUMENT. The
// `int` converts to the `int?` formal (§7's reference → nullable conversion)
// and the primary is INAPPLICABLE, so the `pure` body constructor is invoked
// and the construction is admitted.
BEZEL_TEST(pure_body_primary_ctor_8134,
           primary_plus_pure_body_ctor_nullable_arg_throw_admitted_spec_22_6) {
    auto r = pureCheck(
        "module M;\n"
        "class E1(message: string): Exception(message)\n"
        "  public pure constructor(code: int?) this(\"code\"); end constructor;\n"
        "end E1;\n"
        "class C\n"
        "  public static pure method B(x: int): int\n"
        "    if x < 0 then\n"
        "      throw new E1(x);\n"
        "    end;\n"
        "    return x;\n"
        "  end B;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The AOT arm of that shape: it runs, and the message is "code" — the
// `pure` body constructor's chain.
BEZEL_TEST(pure_body_primary_ctor_8134,
           primary_plus_pure_body_ctor_nullable_arg_throw_runs_spec_22_6) {
    const std::string out = bezel::test::runAotOut(
        "module M;\n"
        "class E1(message: string): Exception(message)\n"
        "  public pure constructor(code: int?) this(\"code\"); end constructor;\n"
        "end E1;\n"
        "class C\n"
        "  public static pure method B(x: int): int\n"
        "    if x < 0 then\n"
        "      throw new E1(x);\n"
        "    end;\n"
        "    return x;\n"
        "  end B;\n"
        "end C;\n"
        "class Program\n"
        "  public static method Main(): int\n"
        "    try\n"
        "      Console.WriteLine(C.B(-1));\n"
        "    catch\n"
        "      var e: E1 => Console.WriteLine(e.Message)\n"
        "    end;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_EQ(out, "code");
}

// A converting REFERENCE argument: the primary `(message: string)` and a
// `pure` body constructor `(a: Animal)`, called with a `Dog` ARGUMENT. `Dog` →
// `Animal` is §12's subtype → supertype conversion, and the primary is
// INAPPLICABLE — so the `pure` body constructor is the invoked one.
BEZEL_TEST(pure_body_primary_ctor_8134,
           primary_plus_pure_body_ctor_supertype_arg_throw_admitted_spec_22_6) {
    auto r = pureCheck(
        "module M;\n"
        "class Animal\n"
        "end Animal;\n"
        "class Dog: Animal\n"
        "end Dog;\n"
        "class E1(message: string): Exception(message)\n"
        "  public pure constructor(a: Animal) this(\"code\"); end constructor;\n"
        "end E1;\n"
        "class C\n"
        "  public static pure method B(d: Dog): int\n"
        "    throw new E1(d);\n"
        "  end B;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =========================================================================
// THE GENERIC REASON — §22.6 cannot identify the invoked constructor.
// =========================================================================
//
// The primary `(message: string)` and the `pure` body constructor
// `(code: string)` are BOTH APPLICABLE for a `string` argument, and neither
// §22.6's exact-match ranking nor its defaulted-arity tie-break separates
// them: the invoked constructor is genuinely unidentifiable, so §22.15's
// carve-out cannot prove a `pure` constructor for the call and refuses with
// the GENERIC reason. The reason is TRUE — the compiler's own §22.6 reports
// the same ambiguity ("no overload is unambiguously better (§22.6)").
//
// THIS IS THE ROW THAT RESERVES THE GENERIC REASON. It must not appear where
// the argument merely CONVERTS (the rows above): there §22.6 identifies the
// invoked constructor.
BEZEL_TEST(pure_body_primary_ctor_8134,
           primary_plus_pure_body_ctor_ambiguous_throw_generic_reason_spec_22_6) {
    auto r = pureCheck(
        "module M;\n"
        "class E1(message: string): Exception(message)\n"
        "  public pure constructor(code: string) this(\"code\"); end constructor;\n"
        "end E1;\n"
        "class C\n"
        "  public static pure method B(x: int): int\n"
        "    if x < 0 then\n"
        "      throw new E1(\"neg\");\n"
        "    end;\n"
        "    return x;\n"
        "  end B;\n"
        "end C;\n");
    const bezel::Diagnostic* d = theDiagnosticContaining(r, kGenericReason);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) BEZEL_EXPECT_EQ(d->location.line, 8);
}
