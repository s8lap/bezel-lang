// fix #3628 (§22.15 rule 6 / §25.1 / §17.6, v0.286): narrow #3610 closed §22.15
// rule 6 for the §25 operator EXPRESSION forms (`a - b`, `~a`, the §25.2
// `=`/`<>` overloads). It deliberately left the STATEMENT operator forms open:
// compound assignment (`a += b`) and increment/decrement (`t++`) each invoke a
// §25 overload too (§25.1, §17.6), but neither resolves through
// `findBinaryOperator`/`findUnaryOperator` and neither recorded its resolved
// overload anywhere sema-side, so `PureBodyWalker` had no fact to ask about.
//
// THE RULE, verbatim (§22.15 "The pure body" rule 6, v0.286):
//
//   6. **Calls to pure members.** A call — method, operator, accessor read, or
//      constructor in the admitted positions — is permitted iff the member
//      **declared on the receiver's static type** (or the statically resolved
//      member, for statics and extensions) is `pure`-declared or
//      pure-classified (below).
//
// §25.1, verbatim: "the compound forms desugar mechanically to read-modify-
// write using the underlying binary operator's overload" — `t += b` IS the
// call `T::operator +`. §17.6, verbatim: "the statement `x++` is equivalent
// to `x := T.operator++(x)`" — `t++` IS the call `T::operator ++`. Both are
// calls, and rule 6 governs them exactly as it governs the spelled-out form.
//
// MEASURED on d8e07d7 (the #3610 fix, without this one): both repros from
// issue #3628 compile with exit 0 and NO diagnostic; the executables exit 7
// (compound assignment) and 5 (increment) — the impure operator body's own
// answer, RUN inside a `pure` method.
//
// THE FIX PUBLISHES A FACT THE COMPILER ALREADY COMPUTES, RATHER THAN ADDING A
// SIXTH PREDICATE. `ExprTypecheck.cpp` already has THREE existence/result-type
// checks that resolve the statement forms' overload by hand and discard it:
// the compound-assignment "primitive target, Named RHS" check, the compound-
// assignment "Named target" check (both via `collectInheritedOperators`, the
// SAME §25.2 both-operand + inherited search codegen's own
// `findCompoundOperatorOverload` repeats to pick the operator it calls at
// runtime), and the IncrementStmt overload-existence check (direct members
// only, matching codegen's IncrementStmt lowering exactly). Each now PUBLISHES
// its match via `SemaContext::setStmtOperatorSymbol` — a new AST-keyed side
// table (`stmtOperatorSymbols_`, keyed on `ast::Stmt*` so one table serves
// both `AssignmentStmt` and `IncrementStmt`) that mirrors the existing
// `exprSymbols_`/`patternSymbols_` shape. `PureBodyWalker`'s `AssignmentStmt`/
// `IncrementStmt` arms read it and call the SAME `checkOperatorOverloadPurity`
// / `callTargetIsPure` narrow #3610 already added — no new spelling of "is
// this member pure" (that duplication is what generated #3174, #3609, and
// #3610 in this very rule).
//
// Unlike `patternSymbols_`/`lambdaParams_`, `stmtOperatorSymbols_` needs no
// entry in ModuleLoader's body-reconstruction remap loops: `checkPureBodies`
// (like `typecheckExpressions`) runs once per compiled `SourceFile` on the
// driver's own `SemaContext`, never on a ModuleLoader-reparsed imported
// generic body — see the accessor's comment in `src/sema/SemaContext.hpp`.
//
// §6b EXCLUSION DIFF — what was ADMITTED before vs what is REJECTED now.
// Before this fix, `PureBodyWalker`'s `AssignmentStmt`/`IncrementStmt` arms
// asked nothing about the resolved operator at all — every compound-assign
// and increment/decrement through a §25 overload was silently admitted
// (§22.15 rule 2 already admits mutating a record local, so nothing else in
// the walker caught it). The diff runs in the REJECT direction and is exactly
// the set of statements for which the ExprTypecheck.cpp sites above resolve a
// MethodSymbol whose `memberIsPure` is false:
//   (A) `t += b` through a §25.1/§25.2 binary overload declared on either
//       operand's Named type — DIRECTLY, through class inheritance
//       (`collectInheritedOperators`), or on a PRIMITIVE target paired with a
//       Named RHS declaring the applicable overload (§25.2's "either
//       position").
//   (B) `t++`/`t--` through a §17.6/§25 unary overload.
//   (C) The same, cross-module (§2.8.2) — the operator's `pure`-ness rides
//       `MetaMember::isPure` exactly as the expression forms' does (narrow
//       #3610's X1/X2 rows); nothing about this fix is import-specific.
// ADMITS, unchanged: `:=` (not a compound form); `??=` (resolves no 2-param
// `operator`, §17.12); every compound form on a PRIMITIVE/PRIMITIVE operand
// pair (`i += 1`) and on `string` (`s += "x"`) — no branch of
// `ExprTypecheck.cpp`'s existence checks ever resolves a symbol for these, so
// `stmtOperatorSymbol` stays null and `checkOperatorOverloadPurity`'s `!ms`
// guard (added by narrow #3610) returns without reporting; an event
// `+=`/`-=` (§24.4) — those never reach ANY of the three existence-check
// branches at all (`!eventPayload` gates entry), so `setStmtOperatorSymbol`
// is never called for one, structurally, not by a new predicate deciding so.
// Pinned by `compound_forms_without_user_overload_accepted_in_pure_body`.
//
// FALSE-REJECT COST: 0. Every in-tree `.bzl` (see the lane's sweep log) was
// compiled with a PARENT-d8e07d7 `bezelc` and with the fixed one (`-c`, so no
// `Main` is required) and BOTH the exit status AND the full diagnostic text
// compared; 0 files differ. Method mirrors narrow #3610's.
//
// Spec: §22.15 "The pure body" rule 6, §25 operator overloading, §25.1
// compound assignment, §25.2 equality overloads, §17.6 the increment and
// decrement operators, §2.8.2 the artifact interface, §24.4 events. All
// v0.286. Related: #3610 (the expression-form fix this one completes), #3609
// (`memberIsPure`), #3174, #3630 (rule 6's accessor-read arm, separately
// blocked on #3618 — untouched here).

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

// ---------------------------------------------------------------------------
// A1 / A2 — compound assignment through a directly-declared §25.1 overload.
// `Acc::operator +` is the impure one under test; `mark` (`""` / `"pure "`)
// is the ONLY variable between the reject and accept twins.
// ---------------------------------------------------------------------------

std::string compoundAssignLocal(const char* mark) {
    return std::string(
        "module App; "
        "public record Acc(V: int) "
        "  public static ") + mark +
        "method operator +(a: Acc, b: Acc): Acc => new Acc(a.V + b.V); "
        "end Acc; "
        "public class Program "
        "  public static pure method P(a: Acc, b: Acc): int "
        "    var t := a; "
        "    t += b; "
        "    return t.V; "
        "  end P; "
        "  public static method Main(): int "
        "    var u := new Acc(5); "
        "    var v := new Acc(2); "
        "    return P(u, v); "
        "  end Main; "
        "end Program; ";
}

// ---------------------------------------------------------------------------
// B1 / B2 — the increment/decrement half (§17.6 / §25).
// ---------------------------------------------------------------------------

std::string incrementLocal(const char* mark) {
    return std::string(
        "module App; "
        "public record C(V: int) "
        "  public static ") + mark +
        "method operator ++(a: C): C => new C(a.V + 1); "
        "end C; "
        "public class Program "
        "  public static pure method P(a: C): int "
        "    var t := a; "
        "    t++; "
        "    return t.V; "
        "  end P; "
        "  public static method Main(): int "
        "    return P(new C(4)); "
        "  end Main; "
        "end Program; ";
}

// ---------------------------------------------------------------------------
// D1 / D2 — an INHERITED §25.1 overload reached via `collectInheritedOperators`
// (class chain), the case the narrower direct-members-only "result type"
// check in ExprTypecheck.cpp would MISS. `Base::operator +` cannot itself be
// marked `pure` and still compile (a `pure` member cannot construct a
// non-trivial-dispose class instance in general position, §22.15 — orthogonal
// to this fix), so the twin here differs by the CALLER's purity instead of
// the operator's: the same impure inherited operator is called from a `pure`
// method (rejected) and a non-`pure` one (accepted, §22.15 governs `pure`
// bodies only).
// ---------------------------------------------------------------------------

std::string compoundAssignInheritedDecls() {
    return
        "public class Base "
        "  public V: int; "
        "  public constructor(v: int) "
        "    V := v; "
        "  end constructor; "
        "  public static method operator +(a: Base, b: Base): owned Base => "
        "new Base(a.V + b.V); "
        "end Base; "
        "public class Derived : Base "
        "  public constructor(v: int) "
        "    base(v); "
        "  end constructor; "
        "end Derived; ";
}

std::string compoundAssignInherited(const char* callerMark) {
    return std::string("module App; ") + compoundAssignInheritedDecls() +
        "public class Program "
        "  public static " + callerMark +
        "method P(a: Derived, b: Derived): int "
        "    var t := a; "
        "    t += b; "
        "    return t.V; "
        "  end P; "
        "  public static method Main(): int "
        "    return P(new Derived(5), new Derived(2)); "
        "  end Main; "
        "end Program; ";
}

// ---------------------------------------------------------------------------
// E1 / E2 — a PRIMITIVE target paired with a Named RHS declaring the
// applicable overload (§25.2 "either position"): `n *= scale` where
// `operator *(int, Scale): int` lives on `Scale`. This is the "primitive
// target, Named RHS" existence-check branch — a separate branch from D1/D2's
// "Named target" one, and separately instrumented.
// ---------------------------------------------------------------------------

std::string compoundAssignPrimitiveTargetNamedRhs(const char* mark) {
    return std::string(
        "module App; "
        "public record Scale(F: int) "
        "  public static ") + mark +
        "method operator *(a: int, b: Scale): int => a * b.F; "
        "end Scale; "
        "public class Program "
        "  public static pure method P(a: int, b: Scale): int "
        "    var n := a; "
        "    n *= b; "
        "    return n; "
        "  end P; "
        "  public static method Main(): int "
        "    return P(3, new Scale(2)); "
        "  end Main; "
        "end Program; ";
}

// ---------------------------------------------------------------------------
// R — the rule-3 blanket over-rejection control. Every compound form here
// resolves NO user `operator` overload (primitive/primitive, `string`
// concatenation, `??=`), so `stmtOperatorSymbol` stays null on every one of
// these statements and the fix must leave them all compiling. A fix that
// rejected every compound assignment or increment would fail this row while
// still passing every reject row above.
// ---------------------------------------------------------------------------

const char* kBlanketControl =
    "module App; "
    "public class Program "
    "  public static pure method P(): int "
    "    var i := 4; "
    "    i += 1; "                 // primitive/primitive compound: i = 5
    "    var s := \"ab\"; "
    "    s += \"x\"; "             // string concatenation: s.Length = 3
    "    var x: int? := null; "
    "    x ??= 9; "                // null-coalescing assign: x = 9
    "    var c := 5; "
    "    c++; "                    // built-in numeric increment: c = 6
    "    return i + s.Length + (x ?? 0) + c; "  // 5 + 3 + 9 + 6 = 23
    "  end P; "
    "  public static method Main(): int => P(); "
    "end Program; ";

// ---------------------------------------------------------------------------
// Cross-module driver (X1/X2). Mirrors narrow #3610's `runXmod3610`, with
// `std::cerr` captured so the rejection can be asserted on its TEXT.
// ---------------------------------------------------------------------------

const std::filesystem::path& wtDir3628() { return wtScratchDir("wt-3628"); }

struct TempFile3628 {
    std::string path;
    explicit TempFile3628(std::string p) : path(std::move(p)) {}
    ~TempFile3628() { std::remove(path.c_str()); }
};

void write3628(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct XmodResult {
    bool built = false;
    int exitCode = -1;        // meaningful only when built
    std::string diagnostics;  // captured compiler stderr
};

XmodResult runXmod3628(const std::string& libSrc, const std::string& mainSrc) {
    XmodResult out;
    auto dir = wtDir3628();
    std::string stem = unique_stem("bezel_pureop3628");
    TempFile3628 lSrc((dir / (stem + "_lib.bzl")).string());
    TempFile3628 lObj((dir / (stem + "_lib.o")).string());
    TempFile3628 mSrc((dir / (stem + "_main.bzl")).string());
    TempFile3628 mExe((dir / (stem + "_main")).string());

    write3628(lSrc.path, libSrc);
    write3628(mSrc.path, mainSrc);

    std::ostringstream captured;
    std::streambuf* saved = std::cerr.rdbuf(captured.rdbuf());

    bezel::Driver driver;
    bool ok = driver.compileObject(lSrc.path, lObj.path).success;
    if (ok) {
        bezel::Dependency dep;
        dep.metadataPath = lObj.path;
        dep.objectPath   = lObj.path;
        std::vector<bezel::Dependency> deps{dep};
        ok = driver.compileFile(mSrc.path, mExe.path, deps).success;
    }

    std::cerr.rdbuf(saved);
    out.diagnostics = captured.str();
    if (!ok) return out;

    out.built = true;
    int status = std::system(mExe.path.c_str());
    if (status != -1 && WIFEXITED(status)) out.exitCode = WEXITSTATUS(status);
    return out;
}

std::string xmodLib(const char* mark) {
    return std::string(
        "module OpLib; "
        "public record Acc(V: int) "
        "  public static ") + mark +
        "method operator +(a: Acc, b: Acc): Acc => new Acc(a.V + b.V); "
        "end Acc; ";
}

const char* kXmodApp =
    "module App; import OpLib; "
    "public class Program "
    "  public static pure method P(a: Acc, b: Acc): int "
    "    var t := a; "
    "    t += b; "
    "    return t.V; "
    "  end P; "
    "  public static method Main(): int "
    "    var u := new Acc(5); "
    "    var v := new Acc(2); "
    "    return P(u, v); "
    "  end Main; "
    "end Program; ";

// The §22.15 diagnostic's stable parts, mirroring narrow #3610's
// `sawPureOperatorReject`. The operator token is asserted separately per row
// so a branch-order regression that reported the WRONG operator is caught.
bool sawPureOperatorReject(const std::string& text, const char* opTok) {
    const std::string needle =
        std::string("call to `operator ") + opTok +
        "` is not admitted in a pure body";
    return text.find(needle) != std::string::npos &&
           text.find("calls-to-pure-members rule") != std::string::npos &&
           text.find("\xC2\xA7""22.15") != std::string::npos;
}

std::string diagText(const test::JitResult& r) {
    std::string all;
    for (const auto& d : r.diagnostics.diagnostics()) all += d.message + "\n";
    return all;
}

}  // namespace

// ===========================================================================
// A1 / A2 — compound assignment, THE FILED DEFECT (issue #3628 measurement 1).
// Pre-fix: compiled clean, program exited 7 — the impure `operator +` body
// RAN inside the `pure` method `P`.
// ===========================================================================

BEZEL_TEST(pure_operator_rule6_statement_3628,
          compound_assign_impure_operator_rejected) {
    auto r = test::runAot(compoundAssignLocal(""));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(sawPureOperatorReject(diagText(r), "+"));
}

BEZEL_TEST(pure_operator_rule6_statement_3628,
          compound_assign_pure_operator_accepted) {
    auto r = test::runAot(compoundAssignLocal("pure "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ===========================================================================
// B1 / B2 — increment, THE FILED DEFECT (issue #3628 measurement 2). Pre-fix:
// compiled clean, program exited 5 — the impure `operator ++` body RAN.
// ===========================================================================

BEZEL_TEST(pure_operator_rule6_statement_3628,
          increment_impure_operator_rejected) {
    auto r = test::runAot(incrementLocal(""));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(sawPureOperatorReject(diagText(r), "++"));
}

BEZEL_TEST(pure_operator_rule6_statement_3628,
          increment_pure_operator_accepted) {
    auto r = test::runAot(incrementLocal("pure "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// ===========================================================================
// D1 / D2 — an INHERITED compound-assign overload. Proves the fix reads the
// SAME `collectInheritedOperators` resolution codegen's
// `findCompoundOperatorOverload` uses, not a direct-members-only rescan that
// would silently miss a base-declared operator.
// ===========================================================================

BEZEL_TEST(pure_operator_rule6_statement_3628,
          compound_assign_inherited_operator_pure_caller_rejected) {
    auto r = test::runAot(compoundAssignInherited("pure "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(sawPureOperatorReject(diagText(r), "+"));
}

BEZEL_TEST(pure_operator_rule6_statement_3628,
          compound_assign_inherited_operator_nonpure_caller_accepted) {
    auto r = test::runAot(compoundAssignInherited(""));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ===========================================================================
// E1 / E2 — a PRIMITIVE target, Named RHS overload (§25.2 "either position"),
// the compound-assignment existence-check branch D1/D2 does not exercise.
// ===========================================================================

BEZEL_TEST(pure_operator_rule6_statement_3628,
          compound_assign_primitive_target_named_rhs_impure_rejected) {
    auto r = test::runAot(compoundAssignPrimitiveTargetNamedRhs(""));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(sawPureOperatorReject(diagText(r), "*"));
}

BEZEL_TEST(pure_operator_rule6_statement_3628,
          compound_assign_primitive_target_named_rhs_pure_accepted) {
    auto r = test::runAot(compoundAssignPrimitiveTargetNamedRhs("pure "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 6);
}

// ===========================================================================
// R — the rule-3 blanket over-rejection control. Passes on BOTH sides of the
// fix; it is here so that "reject every compound assignment/increment" cannot
// masquerade as a fix.
// ===========================================================================

BEZEL_TEST(pure_operator_rule6_statement_3628,
          compound_forms_without_user_overload_accepted_in_pure_body) {
    auto r = test::runAot(kBlanketControl);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 23);
}

// ===========================================================================
// X1 / X2 — the artifact boundary (§2.8.2). The operator's `pure` rides
// `MetaMember::isPure` exactly as narrow #3610's expression-form rows proved;
// nothing about the statement-form resolution added here is import-specific.
// ===========================================================================

BEZEL_TEST(pure_operator_rule6_statement_3628,
          imported_impure_compound_assign_operator_rejected) {
    auto r = runXmod3628(xmodLib(""), kXmodApp);
    BEZEL_EXPECT_FALSE(r.built);
    BEZEL_EXPECT_TRUE(sawPureOperatorReject(r.diagnostics, "+"));
}

BEZEL_TEST(pure_operator_rule6_statement_3628,
          imported_pure_compound_assign_operator_accepted) {
    auto r = runXmod3628(xmodLib("pure "), kXmodApp);
    BEZEL_EXPECT_TRUE(r.built);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

}  // namespace bezel::module
