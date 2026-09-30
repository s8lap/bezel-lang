// fix #3174 (§22.15 "Pure members", v0.254) — a `pure` constructor whose
// `base(…)` chain target lives in an IMPORTED class. Family M8 of umbrella
// #3149/#3152; see docs/proposals/member-symbol-audit-2026-08-13.md.
//
// §22.15 (v0.254), verbatim: "A pure constructor's `base(…)`/`this(…)` chain
// target must itself be pure."
//
// WHAT THE DEFECT WAS. `PureBodyWalker::checkPureCtorChainPurity` resolved the
// chain target by casting each candidate `ConstructorSymbol`'s `decl` to an
// `ast::ConstructorDecl*` and `continue`ing when the cast came back null.
// `ModuleLoader` attaches a re-parsed `ast::ConstructorDecl` only for a GENERIC
// type or for a RECORD, so for a NON-GENERIC imported base class every
// candidate is decl-less: the loop found nothing, and the function's
// "no matching ctor — admit silently" early-out ran. §22.15's rule simply did
// not exist across a module boundary.
//
// MEASURED PRE-FIX on `ec6a02f` (`bezelc -c lib.bzl -o lib.o`, then
// `bezelc --import lib.o -o app app.bzl`):
//
//     imported impure base ctor + local `pure constructor` -> compile rc=0
//     the character-identical single-module twin           -> compile rc=1,
//       "constructor chain call is not admitted in a pure body — base(...)
//        target is not pure — a pure constructor's chain target must itself be
//        declared `pure` (§22.15)"
//
// The only difference between the two programs is the module boundary. This is
// a FALSE ACCEPT: a `pure` contract that is simply false for that program — the
// program then compiles with `pure` licensing whatever a pure region licenses,
// over a constructor that writes to the console.
//
// ⚠ WHY THE ROWS ASSERT ON DIAGNOSTIC TEXT, NOT ON "did it build". The pre-fix
// outcome was a clean build, so a reject row that only checked `rc == -1` would
// pass on a crash or on an unrelated rejection. Every reject row below pins the
// §22.15 chain-purity sentence.
//
// ⚠ THE OVER-REJECTION CONTROLS ARE HALF THE TEST. This fix moves in the REJECT
// direction, so a test set that only pins what must be rejected would also pass
// if the compiler rejected everything. T3/T3b (imported PURE base ctor) and
// T4/T4b (imported base, non-pure DERIVED ctor) are the rows that fail if the
// new predicate is over-broad.
//
// ⚠ #3174's "Related, same function, opposite direction" section is refuted FOR
// THE SHAPES T5/T6 USE, and only those. It predicted that
// `findUserCtorByArity`'s identical null-`decl` skip produces a false REJECT for
// an imported body record with a `pure` ctor and a false ACCEPT for an imported
// positional record carrying a shadowing impure ctor. Probed against twins,
// both AGREE across the boundary — for a constructor whose parameters are
// primitive, which is what these rows declare.
//
// ⚠⚠ THAT IS A SAMPLE, NOT A GENERAL RESULT, and an earlier revision of this
// comment presented it as one ("both AGREE across the boundary today, because
// `ModuleLoader`'s ctor-body re-parse gate admits EVERY record"). The gate
// condition is quoted correctly and #2525 does carry `ctorBodyText` for every
// record — but ATTACHMENT CAN STILL FAIL. `parseAndAttachCtorBody` runs
// `resolveTypes` over a synthetic source declaring only the one type, so a
// constructor naming a SIBLING type does not resolve (#2290 bug 2 / #710) and
// `decl` stays null. Measured on this tree: `record BodyRec2` with
// `pure constructor(s: Sib)` is a FALSE REJECT, and a positional record with an
// impure shadowing ctor naming a sibling is a §22.15 FALSE ACCEPT. **Both
// defects are live while T5/T6 stay green**, which is exactly why the claim is
// now scoped. Filed as #3219.
//
// The rows below are still correct regression pins on the behaviour they
// exercise — nothing about their assertions changed. What changed is that they
// no longer stand in for a general refutation, and they are no longer the
// reason this commit left `findUserCtorByArity` unconverted: that decline now
// rests on the narrower ambiguity argument alone (see the comment on the
// function itself), which #3219 records as weak.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"  // #3156: the ONE sanitizer-honouring compileFile wrapper
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile3174 {
    std::string path;
    explicit TempFile3174(std::string p) : path(std::move(p)) {}
    ~TempFile3174() { std::remove(path.c_str()); }
};

void writeSource3174(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// The dependency library.
//   ImpureBase — the §22.15 chain target that is NOT pure. Its body writes to
//                the console, so "the chain target runs an effect" is a fact
//                about the program, not a modifier-only technicality.
//   PureBase   — the same shape, `pure`-declared, for the over-rejection
//                control. Its body is a birth-store only (§22.15's pure-ctor
//                rule), which is what makes `pure` legal on it.
//   BodyRec / ImpureBodyRec / PosShadow — the three record shapes #3174's
//                "Related" section predicted would misbehave. They are here to
//                pin that they do not.
const char* kLib3174 =
    "module PureChainLib3174; "
    "public class ImpureBase "
    "  public property N: int := 0; "
    "  public constructor(n: int) "
    "    Console.WriteLine(\"impure base ctor ran\"); "
    "    N := n; "
    "  end constructor; "
    "end ImpureBase; "
    "public class PureBase "
    "  public property N: int := 0; "
    "  public pure constructor(n: int) "
    "    this.N := n; "
    "  end constructor; "
    "end PureBase; "
    "public record BodyRec "
    "  public readonly property X: int; "
    "  public pure constructor(x: int) "
    "    this.X := x; "
    "  end constructor; "
    "end BodyRec; "
    "public record ImpureBodyRec "
    "  public readonly property X: int; "
    "  public constructor(x: int) "
    "    this.X := x; "
    "  end constructor; "
    "end ImpureBodyRec; "
    "public record PosShadow(A: int) "
    "  public constructor(a: long) "
    "    Console.WriteLine(\"shadow ctor ran\"); "
    "    this.A := 2; "
    "  end constructor; "
    "end PosShadow;";

// The single-module twin of kLib3174 — character-identical declarations, no
// `import` anywhere. The ONLY difference from an imported row is the module
// boundary, which is what makes a divergence a cross-module defect rather than
// a §22.15 enforcement bug.
const char* kLocalDecls3174 =
    "public class ImpureBase "
    "  public property N: int := 0; "
    "  public constructor(n: int) "
    "    Console.WriteLine(\"impure base ctor ran\"); "
    "    N := n; "
    "  end constructor; "
    "end ImpureBase; "
    "public class PureBase "
    "  public property N: int := 0; "
    "  public pure constructor(n: int) "
    "    this.N := n; "
    "  end constructor; "
    "end PureBase; "
    "public record BodyRec "
    "  public readonly property X: int; "
    "  public pure constructor(x: int) "
    "    this.X := x; "
    "  end constructor; "
    "end BodyRec; "
    "public record ImpureBodyRec "
    "  public readonly property X: int; "
    "  public constructor(x: int) "
    "    this.X := x; "
    "  end constructor; "
    "end ImpureBodyRec; "
    "public record PosShadow(A: int) "
    "  public constructor(a: long) "
    "    Console.WriteLine(\"shadow ctor ran\"); "
    "    this.A := 2; "
    "  end constructor; "
    "end PosShadow;";

std::string appSource3174(const std::string& body, const std::string& tag) {
    return "module PureChainApp3174" + tag + "; import PureChainLib3174; " +
           body;
}

std::string sameModuleProgram3174(const std::string& body,
                                  const std::string& tag) {
    return "module PureChainCtl3174" + tag + "; " +
           std::string(kLocalDecls3174) + " " + body;
}

// Compile the dependency, compile the app against it, run the app in a SEPARATE
// process (per tests/README.md). Returns the child's exit status, or:
//   -1 compile/link/run failure (the app did not build)
//   -2 the dependency itself could not be produced (no toolchain)
// `capturedStderr`, when non-null, receives the app compile's stderr — the only
// way to assert on diagnostic TEXT.
int runImported3174(const std::string& body, const std::string& tag,
                    std::string* capturedStderr = nullptr) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3174_" + tag + "_" + unique_stem("");

    TempFile3174 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3174 aObj((dir / (stem + "_a.o")).string());
    writeSource3174(aSrc.path, kLib3174);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile3174 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3174 bExe((dir / (stem + "_b")).string());
    TempFile3174 bErr((dir / (stem + "_b.err")).string());
    writeSource3174(bSrc.path, appSource3174(body, tag));

    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);

    bool compiled = false;
    if (capturedStderr) {
        std::fflush(stderr);
        int saved = ::dup(STDERR_FILENO);
        int sink = ::open(bErr.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (sink >= 0) {
            ::dup2(sink, STDERR_FILENO);
            ::close(sink);
        }
        compiled = bezel::test::compileFileForTest(driver, bSrc.path, bExe.path,
                                                   deps).success;
        std::fflush(stderr);
        if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }
        std::ifstream in(bErr.path);
        capturedStderr->assign((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    } else {
        compiled = bezel::test::compileFileForTest(driver, bSrc.path, bExe.path,
                                                   deps).success;
    }
    if (!compiled) return -1;
    if (!std::ifstream(bExe.path).good()) return -1;

    pid_t pid = ::fork();
    if (pid == 0) {
        ::execl(bExe.path.c_str(), bExe.path.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) return 1000 + WTERMSIG(status);  // 1011 = SIGSEGV
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Compile and run a SINGLE-module program — the differential control twin.
int runSingleModule3174(const std::string& src, const std::string& tag,
                        std::string* capturedStderr = nullptr) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3174ctl_" + tag + "_" + unique_stem("");
    TempFile3174 sSrc((dir / (stem + ".bzl")).string());
    TempFile3174 sExe((dir / stem).string());
    TempFile3174 sErr((dir / (stem + ".err")).string());
    writeSource3174(sSrc.path, src);

    bezel::Driver driver;
    bool compiled = false;
    if (capturedStderr) {
        std::fflush(stderr);
        int saved = ::dup(STDERR_FILENO);
        int sink = ::open(sErr.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (sink >= 0) {
            ::dup2(sink, STDERR_FILENO);
            ::close(sink);
        }
        compiled = bezel::test::compileFileForTest(driver, sSrc.path, sExe.path,
                                                   {}).success;
        std::fflush(stderr);
        if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }
        std::ifstream in(sErr.path);
        capturedStderr->assign((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    } else {
        compiled = bezel::test::compileFileForTest(driver, sSrc.path, sExe.path,
                                                   {}).success;
    }
    if (!compiled) return -1;
    if (!std::ifstream(sExe.path).good()) return -1;

    pid_t pid = ::fork();
    if (pid == 0) {
        ::execl(sExe.path.c_str(), sExe.path.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) return 1000 + WTERMSIG(status);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

bool mentions3174(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// The §22.15 chain-purity sentence, as the diagnostic renders it.
const char* kChainDiag3174 =
    "target is not pure — a pure constructor's chain target must itself be "
    "declared `pure`";

// A derived class whose `pure constructor` chains to `Base`, plus a Main.
std::string derivedProgram3174(const char* baseName, const char* pureKeyword,
                               const std::string& tag) {
    return std::string("public class Derived3174") + tag + ": " + baseName +
           " " +
           "  public " + pureKeyword + " constructor(n: int) "
           "    base(n); "
           "  end constructor; "
           "end Derived3174" + tag + "; "
           "public class C3174" + tag + " "
           "  public static method Main(): int => 0; "
           "end C3174" + tag + ";";
}

}  // namespace

// ===========================================================================
// T1 — §22.15 SOUNDNESS: a local `pure constructor` chaining `base(n)` to an
// IMPORTED IMPURE base constructor is REJECTED, naming the §22.15 rule.
//
// This is the issue's headline row. Pre-fix it compiled with exit 0.
// ===========================================================================
BEZEL_TEST(xmod_pure_ctor_chain_3174,
           t1_pure_ctor_chaining_imported_impure_base_rejected_22_15) {
    std::string err;
    int rc = runImported3174(derivedProgram3174("ImpureBase", "pure", "T1"),
                             "t1", &err);
    if (rc == -2) return;  // no toolchain for the dependency
    BEZEL_EXPECT_EQ(rc, -1);  // must NOT build
    BEZEL_EXPECT_TRUE(mentions3174(err, kChainDiag3174));
    BEZEL_EXPECT_TRUE(mentions3174(err, "22.15"));
}

// ===========================================================================
// T1b — the SAME-MODULE twin of T1 (§2.8.3 rule 4). The two spellings must
// agree; this is the differential control that makes T1 a cross-module claim
// rather than a §22.15 enforcement claim.
// ===========================================================================
BEZEL_TEST(xmod_pure_ctor_chain_3174,
           t1b_same_module_pure_ctor_chaining_impure_base_rejected_22_15) {
    std::string err;
    int rc = runSingleModule3174(
        sameModuleProgram3174(derivedProgram3174("ImpureBase", "pure", "T1b"),
                              "T1b"),
        "t1b", &err);
    BEZEL_EXPECT_EQ(rc, -1);
    BEZEL_EXPECT_TRUE(mentions3174(err, kChainDiag3174));
}

// ===========================================================================
// T3 — OVER-REJECTION CONTROL: a local `pure constructor` chaining to an
// IMPORTED **PURE** base constructor must still COMPILE AND RUN.
//
// This is the row that fails if the fix admits nothing rather than the right
// thing. It is also the row that proves `MetaMember::isPure` actually crosses
// the wire for a constructor — the whole fix rests on that channel, and a
// broken channel would look exactly like "the predicate is conservative".
// ===========================================================================
BEZEL_TEST(xmod_pure_ctor_chain_3174,
           t3_pure_ctor_chaining_imported_pure_base_admitted_22_15) {
    std::string err;
    int rc = runImported3174(derivedProgram3174("PureBase", "pure", "T3"),
                             "t3", &err);
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 0);
    BEZEL_EXPECT_TRUE(!mentions3174(err, kChainDiag3174));
}

// ===========================================================================
// T3b — the SAME-MODULE twin of T3.
// ===========================================================================
BEZEL_TEST(xmod_pure_ctor_chain_3174,
           t3b_same_module_pure_ctor_chaining_pure_base_admitted_22_15) {
    std::string err;
    int rc = runSingleModule3174(
        sameModuleProgram3174(derivedProgram3174("PureBase", "pure", "T3b"),
                              "T3b"),
        "t3b", &err);
    BEZEL_EXPECT_EQ(rc, 0);
    BEZEL_EXPECT_TRUE(!mentions3174(err, kChainDiag3174));
}

// ===========================================================================
// T4 — OVER-REJECTION CONTROL: the derived constructor is NOT `pure`, so
// §22.15's chain rule does not apply at all and the impure imported base is
// perfectly legal. `checkPureCtorChainPurity` must not fire.
//
// (`checkOneMember` gates the walk on `cd->isPure`, so this row also pins that
// the fix did not move the gate.)
// ===========================================================================
BEZEL_TEST(xmod_pure_ctor_chain_3174,
           t4_nonpure_derived_ctor_over_impure_imported_base_admitted_22_15) {
    std::string err;
    int rc = runImported3174(derivedProgram3174("ImpureBase", "", "T4"),
                             "t4", &err);
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 0);
    BEZEL_EXPECT_TRUE(!mentions3174(err, kChainDiag3174));
}

// ===========================================================================
// T4b — the SAME-MODULE twin of T4.
// ===========================================================================
BEZEL_TEST(xmod_pure_ctor_chain_3174,
           t4b_same_module_nonpure_derived_ctor_admitted_22_15) {
    std::string err;
    int rc = runSingleModule3174(
        sameModuleProgram3174(derivedProgram3174("ImpureBase", "", "T4b"),
                              "T4b"),
        "t4b", &err);
    BEZEL_EXPECT_EQ(rc, 0);
    BEZEL_EXPECT_TRUE(!mentions3174(err, kChainDiag3174));
}

// ===========================================================================
// T5 — REFUTATION PIN (#3174's "Related" section, first half): record
// construction of an IMPORTED body record in a pure body follows the record
// ctor's OWN purity, and does so identically to its single-module twin.
//
// `BodyRec`'s ctor is `pure` -> admitted. `ImpureBodyRec`'s is not -> rejected.
// #3174 predicted a false REJECT of the first; it does not happen FOR THIS
// SHAPE, because an imported RECORD's `ConstructorSymbol::decl` is attached
// here (ModuleLoader re-parses `ctorBodyText`, carried for every record by
// #2525). It IS attached only when that re-parse succeeds: a ctor naming a
// sibling type fails to resolve and the false reject #3174 predicted does then
// occur — #3219. These rows use primitive-parameter ctors and so never reach
// that path.
// ===========================================================================
BEZEL_TEST(xmod_pure_ctor_chain_3174,
           t5_imported_body_record_pure_ctor_admitted_in_pure_body_22_15) {
    std::string err;
    int rc = runImported3174(
        "public class C3174T5 "
        "  public static pure method Make(x: int): int "
        "    var r := new BodyRec(x); "
        "    return r.X; "
        "  end Make; "
        "  public static method Main(): int => 0; "
        "end C3174T5;",
        "t5", &err);
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 0);
    BEZEL_EXPECT_TRUE(!mentions3174(err, "record construction is not admitted"));
}

// ===========================================================================
// T5b — the impure half of T5: an IMPORTED body record whose ctor is NOT pure
// is REJECTED in a pure body, exactly as its twin is. Without this row T5 alone
// would pass on a compiler that admitted every record construction.
// ===========================================================================
BEZEL_TEST(xmod_pure_ctor_chain_3174,
           t5b_imported_body_record_impure_ctor_rejected_in_pure_body_22_15) {
    std::string err;
    int rc = runImported3174(
        "public class C3174T5b "
        "  public static pure method Make(x: int): int "
        "    var r := new ImpureBodyRec(x); "
        "    return r.X; "
        "  end Make; "
        "  public static method Main(): int => 0; "
        "end C3174T5b;",
        "t5b", &err);
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, -1);
    BEZEL_EXPECT_TRUE(mentions3174(err, "record construction is not admitted"));
    BEZEL_EXPECT_TRUE(mentions3174(err, "22.15"));
}

// ===========================================================================
// T6 — REFUTATION PIN (#3174's "Related" section, second half): an IMPORTED
// POSITIONAL record carrying a shadowing IMPURE constructor of matching arity
// is REJECTED in a pure body — the false ACCEPT #3174 predicted does not occur.
// The shadow's parameter list differs from the primary's (`long` vs `int`) so the
// record is spec-valid under §9.3, v0.308 (a body constructor with the SAME
// parameter list as the primary is rejected; fix #5132); `Make(x: long)` selects
// the impure body constructor by §22.6 overload resolution.
// ===========================================================================
BEZEL_TEST(xmod_pure_ctor_chain_3174,
           t6_imported_positional_record_impure_shadow_ctor_rejected_22_15) {
    std::string err;
    int rc = runImported3174(
        "public class C3174T6 "
        "  public static pure method Make(x: long): int "
        "    var r := new PosShadow(x); "
        "    return r.A; "
        "  end Make; "
        "  public static method Main(): int => 0; "
        "end C3174T6;",
        "t6", &err);
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, -1);
    BEZEL_EXPECT_TRUE(mentions3174(err, "record construction is not admitted"));
}

// ===========================================================================
// T6b — the SAME-MODULE twin of T6.
// ===========================================================================
BEZEL_TEST(xmod_pure_ctor_chain_3174,
           t6b_same_module_positional_record_impure_shadow_ctor_rejected_22_15) {
    std::string err;
    int rc = runSingleModule3174(
        sameModuleProgram3174(
            "public class C3174T6b "
            "  public static pure method Make(x: long): int "
            "    var r := new PosShadow(x); "
            "    return r.A; "
            "  end Make; "
            "  public static method Main(): int => 0; "
            "end C3174T6b;",
            "T6b"),
        "t6b", &err);
    BEZEL_EXPECT_EQ(rc, -1);
    BEZEL_EXPECT_TRUE(mentions3174(err, "record construction is not admitted"));
}

// ===========================================================================
// T7 — the OLD T6 fixture shape (`record PosShadow(A: int)` beside
// `constructor(a: int)`) is a §9.3 (v0.308) clash and is REJECTED; pins why
// t6/t6b's shadow constructor now takes `long`. (fix-forward #5132)
// ===========================================================================
BEZEL_TEST(xmod_pure_ctor_chain_3174,
           t7_positional_record_same_arity_same_types_shadow_ctor_rejected_9_3) {
    std::string err;
    int rc = runSingleModule3174(
        "module PureChainCtl3174T7; "
        "public record PosShadow(A: int) "
        "  public constructor(a: int) "
        "    this.A := a * 2; "
        "  end constructor; "
        "end PosShadow; "
        "public class C3174T7 "
        "  public static method Main(): int => 0; "
        "end C3174T7;",
        "t7", &err);
    BEZEL_EXPECT_EQ(rc, -1);
    BEZEL_EXPECT_TRUE(mentions3174(
        err, "body constructor redeclares the primary constructor"));
    BEZEL_EXPECT_TRUE(mentions3174(err, "§9.3"));
}

}  // namespace bezel::module
