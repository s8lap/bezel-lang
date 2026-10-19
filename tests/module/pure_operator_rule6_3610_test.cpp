// narrow #3610 (§22.15 rule 6 / §25 / §25.2 / §2.8.2, v0.286): §22.15's
// calls-to-pure-members rule was never applied to an OPERATOR invocation. This
// file pins the §25 operator EXPRESSION forms. The statement forms (`a += b`,
// `t++` through an overload) and rule 6's "accessor read" arm are still open —
// #3628 and #3630, both filed with measured repros; #3630 is blocked on #3618.
//
// THE RULE, verbatim (§22.15 "The pure body" rule 6, v0.286):
//
//   6. **Calls to pure members.** A call — method, operator, accessor read, or
//      constructor in the admitted positions — is permitted iff the member
//      **declared on the receiver's static type** (or the statically resolved
//      member, for statics and extensions) is `pure`-declared or
//      pure-classified (below).
//
// WHAT THE DEFECT WAS. `PureBodyWalker::checkExpr`'s BinaryExpr arm implemented
// rule 3 ("All built-in operators on primitives, enums, and `string`") by
// walking the two operands, and classified exactly one operator: `in` — which
// is not an overload at all but §17.5's `Contains` call, added by fix #3279 as
// a hand-written receiver-side scan. Not one §25 user-declared overload was
// ever asked about, on either the binary or the unary path. A `pure` body
// therefore CALLED a non-`pure` `operator -` and RAN it: measured on the parent
// commit, the L1 program below compiled with exit 0 and no diagnostic, and the
// executable exited 3 — the impure operator body's own answer. The cross-module
// twin (X1) behaved identically, so this is a plain §22.15 gap and not a seam
// gap.
//
// THE FIX ASKS THE FACT THE COMPILER ALREADY COMPUTES. `ExprTC::typeBinary` and
// `ExprTC::typeUnary` already RESOLVE the overload and already record it on the
// operator node — `ctx_.setExprSymbol(&be, opm)` / `(&ue, opm)`
// (ExprTypecheck_Exprs.cpp:4688, :4809, :5894) — and nothing else ever puts a
// symbol on a BinaryExpr/UnaryExpr node, so "this node carries a MethodSymbol"
// IS "this operator is a §25 user-declared overload". The purity question then
// goes to `PureBodyWalker::callTargetIsPure`, which fix #3609 had just made the
// one authority (`sema::memberIsPure`). No sixth spelling of "is this member
// pure" was added — that duplication is what generated #3174 and #3609 in this
// very rule.
//
// WHY EVERY ROW HERE IS A PAIR. The fix moves in the REJECT direction, so a
// test set that only pinned rejections would be passed by a compiler that
// rejected every operator. Each reject row below has a twin that differs from
// it by exactly the five characters `pure ` on the operator declaration (L2,
// L4, L6, C2, X2) — those must still compile AND produce the right value — plus
// R3, a blanket rule-3 control carrying every built-in operator form
// (int/uint/decimal/double/char/bool/string arithmetic, comparison, bitwise
// and shift; enum and record auto-equality; a counted `for`; `in` over a
// Range) inside `pure` bodies.
//
// FALSE-REJECT COST: 0. Every in-tree `.bzl` (2863 files) was compiled by a
// PARENT-commit `bezelc` and by the fixed one and BOTH the exit status and the
// full diagnostic text compared; 0 files differ. See the commit body.
//
// ⚠ Not a plain `runAot` file throughout: X1/X2's observable is cross-module and
// `runAot` compiles with `deps={}`. Those two rows drive the same real Driver
// path (compileObject → compileFile → link → native exec), one process per
// program, with `std::cerr` captured so the cross-module rejection asserts on
// its TEXT rather than on "did not build".
//
// Spec: §22.15 "The pure body" rules 3 and 6, §22.15 "Checking and dispatch",
// §25 operator overloading, §25.2 equality overloads, §2.8.2 the artifact
// interface. All v0.286. Related: #3279 (the `in` special case), #3609
// (`memberIsPure`), #3618 (the accessor-level `pure get` wire slot — see the
// note at N3 below), #2052 / #2061 (the pure-body slices).

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
// Sources. Every reject/accept pair is generated from ONE text with `mark`
// as the only variable, so the twins are character-identical but for `pure `.
// ---------------------------------------------------------------------------

// §25 binary overload on a class. `-` is the impure one under test; the class
// carries nothing else, so nothing but the marker can explain a verdict change.
std::string binDecls(const char* mark) {
    return std::string(
        "public class Vec "
        "  public X: int; "
        "  public constructor(x: int) "
        "    X := x; "
        "  end constructor; "
        "  public static ") + mark +
        "method operator -(a: Vec, b: Vec): int => a.X - b.X; "
        "end Vec; ";
}

// The pure body that invokes it, plus a `Main` that makes the answer OBSERVABLE:
// 5 - 2 = 3 is what the impure operator body returns, and it is what the
// pre-fix compiler let the program print through its exit status.
const char* kBinBody =
    "public class Program "
    "  public static pure method P(a: Vec, b: Vec): int "
    "    return a - b; "
    "  end P; "
    "  public static method Main(): int "
    "    var u := new Vec(5); "
    "    var v := new Vec(2); "
    "    return P(u, v); "
    "  end Main; "
    "end Program; ";

std::string binLocal(const char* mark) {
    return "module App; " + binDecls(mark) + kBinBody;
}
std::string binLib(const char* mark) {
    return "module OpLib; " + binDecls(mark);
}
std::string binApp() {
    return std::string("module App; import OpLib; ") + kBinBody;
}

// §25 UNARY overload on a record. `~a` = a.V + 1 = 5 for V = 4.
std::string unLocal(const char* mark) {
    return std::string(
        "module App; "
        "public record R(V: int) "
        "  public static ") + mark +
        "method operator ~(a: R): int => a.V + 1; "
        "end R; "
        "public class Program "
        "  public static pure method P(a: R): int "
        "    return ~a; "
        "  end P; "
        "  public static method Main(): int "
        "    return P(new R(4)); "
        "  end Main; "
        "end Program; ";
}

// §25.2 equality overloads on a record. §25.2 requires the `Equals(T)` +
// `override GetHashCode` pair alongside a user `=`, so they are here for that
// rule, not for this one — neither is called from the pure body.
std::string eqLocal(const char* mark) {
    return std::string(
        "module App; "
        "public record Box(V: int) "
        "  public static ") + mark +
        "method operator =(a: Box, b: Box): bool => a.V = b.V; "
        "  public static " + mark +
        "method operator <>(a: Box, b: Box): bool => a.V <> b.V; "
        "  public method Equals(other: Box): bool => V = other.V; "
        "  public override method GetHashCode(): int => V; "
        "end Box; "
        "public class Program "
        "  public static pure method P(a: Box, b: Box): int "
        "    if a = b then return 11; end; "
        "    return 22; "
        "  end P; "
        "  public static method Main(): int "
        "    return P(new Box(5), new Box(5)); "
        "  end Main; "
        "end Program; ";
}

// §22.16 CLAUSE TYPING. "Each clause's expression … is admitted under the
// pure-body discipline of §22.15 with the amendments of this section", and the
// same walker runs a clause in `clauseMode_` — so the operator arm reaches
// `requires` / `ensures` / `invariant` too. §25.2 requires the comparison
// operators to be overloaded as a complete group, which is why all four are
// declared; only `<` is called. Pre-fix the clause compiled.
std::string clauseLocal(const char* mark) {
    return std::string(
        "module App; "
        "public record Box(V: int) "
        "  public static ") + mark +
        "method operator <(a: Box, b: Box): bool => a.V < b.V; "
        "  public static " + mark +
        "method operator >(a: Box, b: Box): bool => a.V > b.V; "
        "  public static " + mark +
        "method operator <=(a: Box, b: Box): bool => a.V <= b.V; "
        "  public static " + mark +
        "method operator >=(a: Box, b: Box): bool => a.V >= b.V; "
        "end Box; "
        "public class Program "
        "  public static method M(a: Box, b: Box): int "
        "    requires(a < b) "
        "    return 9; "
        "  end M; "
        "  public static method Main(): int "
        "    return M(new Box(1), new Box(2)); "
        "  end Main; "
        "end Program; ";
}

// THE RULE-3 BLANKET CONTROL. Every built-in operator form §22.15 rule 3 admits
// ("All built-in operators on primitives, enums, and `string`", plus record and
// enum equality and the counted `for` of rule 5), all inside `pure` bodies.
// None of these resolves a MethodSymbol, so the fix must leave every one of
// them alone; a fix that classified operators by TOKEN instead of by resolved
// overload fails here. 18 is the sum the program computes.
const char* kRule3Control =
    "module App; "
    "public enum Color Red, Green, Blue; end Color; "
    "public record Pt(X: int, Y: int); "
    "public class Program "
    "  public static pure method Prims(a: int, b: int, c: decimal, d: double, "
    "s: string, u: uint, ch: char, bo: bool): int "
    "    var t := a + b - a * b; "
    "    var m := a mod b; "
    "    var q := (a & b) | (a ^ b); "
    "    var sh := (a << 1) >> 1; "
    "    var cm := (if a < b then 1 else 0) + (if a >= b then 2 else 0); "
    "    var eq := (if a = b then 1 else 0) + (if a <> b then 2 else 0); "
    "    var dd := c + c; "
    "    var ee := d * d; "
    "    var ss := s + \"x\" + \"y\"; "
    "    var uu := u + u; "
    "    var cc := (if ch = 'a' then 1 else 0); "
    "    var bb := (if bo and not bo then 1 else 0) + (if bo or bo then 1 "
    "else 0); "
    "    var neg := -a; "
    "    var cpl := ~a; "
    "    return t + m + q + sh + cm + eq + ss.Length + cc + bb + neg + cpl; "
    "  end Prims; "
    "  public static pure method Enums(x: Color, y: Color): int "
    "    return (if x = y then 1 else 0) + (if x <> y then 2 else 0); "
    "  end Enums; "
    "  public static pure method Recs(p: Pt, q: Pt): int "
    "    return (if p = q then 1 else 0) + (if p <> q then 2 else 0); "
    "  end Recs; "
    "  public static pure method Rng(a: int): int "
    "    var r := 0; "
    "    for var i in 0..3 do "
    "      r := r + i; "
    "    end; "
    "    return r + (if a in 0..9 then 1 else 0); "
    "  end Rng; "
    "  public static method Main(): int "
    "    return Prims(3, 2, 1m, 1.0, \"s\", 1u, 'a', true) "
    "         + Enums(Color.Red, Color.Blue) "
    "         + Recs(new Pt(1,2), new Pt(1,2)) "
    "         + Rng(1); "
    "  end Main; "
    "end Program; ";

// §22.15 "Checking and dispatch": "calls on constraint-typed values resolve to
// the constraint interface's declared members — none pure in this revision — so
// generic pure bodies handle type-parameter-typed values opaquely (copy, hold,
// return)." stdlib §2.19 declares `INumber<T>`'s operators with no `pure`
// marker, so `a + b` on a `where T: INumber<T>` operand is exactly the call
// rule 6 excludes. Pre-fix it compiled. This row pins the spec's own sentence.
const char* kConstraintOperator =
    "module App; "
    "public class Program "
    "  public static pure method Add<T>(a: T, b: T): T where T: INumber<T> "
    "    return a + b; "
    "  end Add; "
    "  public static method Main(): int "
    "    return Add<int>(3, 4); "
    "  end Main; "
    "end Program; ";

// ---------------------------------------------------------------------------
// Cross-module driver (X1/X2). Mirrors the #3615 helper, with `std::cerr`
// captured so the rejection can be asserted on its TEXT.
// ---------------------------------------------------------------------------

const std::filesystem::path& wtDir3610() { return wtScratchDir("wt-3610"); }

struct TempFile3610 {
    std::string path;
    explicit TempFile3610(std::string p) : path(std::move(p)) {}
    ~TempFile3610() { std::remove(path.c_str()); }
};

void write3610(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct XmodResult {
    bool built = false;
    int exitCode = -1;      // meaningful only when built
    std::string diagnostics;  // captured compiler stderr
};

// Compile lib -> object, compile main importing lib -> executable, run.
XmodResult runXmod3610(const std::string& libSrc, const std::string& mainSrc) {
    XmodResult out;
    auto dir = wtDir3610();
    std::string stem = unique_stem("bezel_pureop3610");
    TempFile3610 lSrc((dir / (stem + "_lib.bzl")).string());
    TempFile3610 lObj((dir / (stem + "_lib.o")).string());
    TempFile3610 mSrc((dir / (stem + "_main.bzl")).string());
    TempFile3610 mExe((dir / (stem + "_main")).string());

    write3610(lSrc.path, libSrc);
    write3610(mSrc.path, mainSrc);

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

// The §22.15 diagnostic's stable parts. `\xC2\xA7` is the UTF-8 section sign the
// compiler emits; the operator token is asserted separately per row so a
// branch-order regression that reported the WRONG operator would be caught.
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
// L1 / L2 — the filed defect and its one-keyword twin (§25 binary overload).
// ===========================================================================

// #3610 / §22.15 rule 6 — L1, THE DEFECT. Pre-fix: compiled clean and the
// program exited 3, i.e. the non-`pure` `operator -` body RAN inside `P`.
BEZEL_TEST(pure_operator_rule6_3610, local_impure_binary_operator_rejected) {
    auto r = test::runAot(binLocal(""));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(sawPureOperatorReject(diagText(r), "-"));
}

// #3610 — L2, THE OVER-REJECTION CONTROL for L1. The SAME text with `pure `
// on the operator: still compiles, and still answers 3. A fix that rejected
// every §25 overload passes L1 and fails here.
BEZEL_TEST(pure_operator_rule6_3610, local_pure_binary_operator_accepted) {
    auto r = test::runAot(binLocal("pure "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// ===========================================================================
// L3 / L4 — the unary half (§25 unary overload). `typeUnary` records the
// overload on the node exactly as `typeBinary` does; pre-fix the walker's
// UnaryExpr arm only rejected `await`.
// ===========================================================================

BEZEL_TEST(pure_operator_rule6_3610, local_impure_unary_operator_rejected) {
    auto r = test::runAot(unLocal(""));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(sawPureOperatorReject(diagText(r), "~"));
}

BEZEL_TEST(pure_operator_rule6_3610, local_pure_unary_operator_accepted) {
    auto r = test::runAot(unLocal("pure "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// ===========================================================================
// L5 / L6 — the §25.2 equality overloads, which reach the same BinaryExpr arm
// through the `=` / `<>` tokens.
// ===========================================================================

BEZEL_TEST(pure_operator_rule6_3610, local_impure_equality_operator_rejected) {
    auto r = test::runAot(eqLocal(""));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(sawPureOperatorReject(diagText(r), "="));
}

BEZEL_TEST(pure_operator_rule6_3610, local_pure_equality_operator_accepted) {
    auto r = test::runAot(eqLocal("pure "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 11);
}

// ===========================================================================
// R3 — the rule-3 blanket control. Passes on BOTH sides of the fix; it is here
// so that "reject every operator" cannot masquerade as a fix.
// ===========================================================================

BEZEL_TEST(pure_operator_rule6_3610, builtin_operators_in_pure_bodies_accepted) {
    auto r = test::runAot(kRule3Control);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 18);
}

// ===========================================================================
// N3 — §22.15 "Checking and dispatch", the constraint-typed operand. A NEW
// rejection, and the one the spec states in as many words.
// ===========================================================================

BEZEL_TEST(pure_operator_rule6_3610, constraint_static_operator_rejected) {
    auto r = test::runAot(kConstraintOperator);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(sawPureOperatorReject(diagText(r), "+"));
}

// ===========================================================================
// X1 / X2 — the artifact boundary (§2.8.2). The operator's `pure` rides
// `MetaMember::isPure` (ModuleMetadataBuilder.cpp:1227 writes it from
// `ast::OperatorDecl::isPure`; `Loader::convertMember` restores it at
// ModuleLoader.cpp:927 —
// `member->modifiers.push_back(ctx.internString("pure"));`), so BOTH
// verdicts must survive the round trip. ⚠ #3618 records that the
// accessor-level `pure get` has NO such slot — which is why this fix is
// confined to the OPERATOR forms and leaves rule 6's "accessor read" alone:
// tightening accessor reads today would reject a cross-module `p.P` whose
// character-identical single-module twin compiles.
// ===========================================================================

BEZEL_TEST(pure_operator_rule6_3610, imported_impure_binary_operator_rejected) {
    auto r = runXmod3610(binLib(""), binApp());
    BEZEL_EXPECT_FALSE(r.built);
    BEZEL_EXPECT_TRUE(sawPureOperatorReject(r.diagnostics, "-"));
}

BEZEL_TEST(pure_operator_rule6_3610, imported_pure_binary_operator_accepted) {
    auto r = runXmod3610(binLib("pure "), binApp());
    BEZEL_EXPECT_TRUE(r.built);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}


// ===========================================================================
// C1 / C2 — §22.16 contract clauses. The clause is checked under §22.15's
// pure-body rules, so the operator arm governs it; `M` itself is NOT `pure`.
// ===========================================================================

BEZEL_TEST(pure_operator_rule6_3610, impure_operator_in_contract_clause_rejected) {
    auto r = test::runAot(clauseLocal(""));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(sawPureOperatorReject(diagText(r), "<"));
}

BEZEL_TEST(pure_operator_rule6_3610, pure_operator_in_contract_clause_accepted) {
    auto r = test::runAot(clauseLocal("pure "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 9);
}

}  // namespace bezel::module
