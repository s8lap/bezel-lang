// #6350 — the CROSS-MODULE guard for the #6334 fix (#22.6 stage 3 on a lone
// overload candidate). §22.6, §14.5, §5.1, §12.9, §13.2, §2.7, v0.338.
//
// WHAT #6334 DID, AND WHY A RATCHET WAS ALL THAT HELD IT.
// `resolveMethodOverload` (src/sema/ExprTypecheck_Calls.cpp) short-circuited
// on `cands.size() == 1` above the §14.5 phase-1 substitution, so a sole
// candidate returned before anything had substituted its signature: an arity
// match was taken as applicability. 62ccf044 added the check, but gated it on
//
//     const auto* loneMd =
//         dynamic_cast<const ast::MethodDecl*>(cands[0]->decl);
//     if (loneMd && !hasDiscard) { ... }
//
// `ModuleLoader` assigns no `ast::MethodDecl` to an IMPORTED method, so that
// cast is null for EVERY method reached across a module boundary and the whole
// #6334 block silently did not run there — a silent-acceptance inversion
// (umbrella #3152, class-3 seam). It was caught ONLY because
// `scripts/check_imported_symbol_reads.py` went RED on the extra AST read
// (19 reads vs a baseline of 18). 9ef7d389 removed the gate: the check now
// reads `cands[0]->genericParams` (a `MethodSymbol` field, populated on both
// sides) and the already-computed substituted rows, and it takes the §12.9
// variance fact through `sema::hasVariantGenericParam`, which consults the AST
// FIRST and `TypeSymbol::genericParams` second.
//
// WHAT WAS MISSING HERE. The nine rows in
// `tests/sema/lone_candidate_unrelated_argument_6334_test.cpp` are all
// single-module, and `62ccf044`'s two-module coverage was a CLI repro typed by
// hand. So the regression was guarded by a ratchet — a count, not a
// behavioural assertion — and a ratchet cannot say "this call is rejected".
// An import-blind `dynamic_cast` is a REUSABLE mistake: this is the second
// time one has been caught only by the counter. These rows are the behavioural
// pin, across the boundary, for both halves of 9ef7d389's change (the removed
// AST gate on the candidate, and the AST-only §12.9 variance read).
//
// ORACLE. `Driver::compileObject` + `Driver::compileFile` with a real
// `Dependency`, driven in-process exactly as the shipped
// `tests/module/imported_private_member_5025_test.cpp` does — a two-module
// build needs a real artifact on disk, so `runAot` cannot reach it. Reject
// rows assert BOTH that the import compile fails AND that the message is this
// check's own §22.6 stage-3 wording; a compile that failed for some unrelated
// reason (a private type, a missing export) would not satisfy them. Accept
// rows link and EXECUTE the importer and read the value the library's body
// returned, so they cannot pass on a zero.
//
// ── WHAT IS NOT ESTABLISHED HERE (stated plainly) ──────────────────────────
//   * The §12.9 ACCEPT row (R5) is a BOUNDARY CONTROL, not a fail-without.
//     It passed three separate src mutations, including reverting the
//     `hasVariantGenericParam` accessor itself; see its own comment for the
//     measurements and for why the gate's premise cannot currently hold.
//   * The same fix-forward dropped a BOUND branch (it called
//     `checkArgsAgainstParams` with the substituted row). That branch is gone
//     for good and has no fail-without case — see the handback — so no row
//     here pins it.
//   * The §14.1 Agreement conflict row (`typeCall`'s #896 message) is NOT
//     repeated across the boundary; it is a `typeCall` behaviour, not an
//     import-gated one, and it is pinned single-module by
//     `lone_candidate_unrelated_argument_6334.
//     agreement_conflict_keeps_the_single_896_diagnostic_spec_14_1`.
//   * The residual of #6334 — whether §14.5 inference sees through a §12
//     subtype step — is still open as spec-question #6342, and none of these
//     rows depends on its answer. Each row below is one where the verdict is
//     certain under BOTH readings.
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile6350 {
    std::string path;
    TempFile6350() = default;
    explicit TempFile6350(std::string p) : path(std::move(p)) {}
    TempFile6350& operator=(std::string p) {
        std::remove(path.c_str());
        path = std::move(p);
        return *this;
    }
    ~TempFile6350() {
        if (!path.empty()) std::remove(path.c_str());
    }
};

void writeSource6350(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// A whole two-module build, owning EVERY temp file for its lifetime. The
// artifact and the executable must still be on disk when the row reads them,
// so all three TempFiles live here rather than in the compile helpers (a
// TempFile local to a helper would delete the artifact on return, and every
// row would then fail for a reason that has nothing to do with this check).
struct XMod6350 {
    TempFile6350 libSrc, libObj, appSrc, appExe;
    bool libCompiled = false;
    bool appCompiled = false;
    std::string err;

    XMod6350(const std::string& libText, const std::string& appText) {
        auto dir = fs::temp_directory_path();
        std::string stem = unique_stem("bz6350_");
        libSrc = (dir / (stem + "_lib.bzl")).string();
        libObj = (dir / (stem + "_lib.o")).string();
        appSrc = (dir / (stem + "_app.bzl")).string();
        appExe = (dir / (stem + "_app")).string();
        writeSource6350(libSrc.path, libText);
        writeSource6350(appSrc.path, appText);
        {
            bezel::Driver driver;
            libCompiled = driver.compileObject(libSrc.path, libObj.path).success;
        }
        // The artifact must EXIST: `compileObject` reports success only if it
        // wrote one, and an importer handed a missing artifact fails for a
        // reason that has nothing to do with this check.
        BEZEL_EXPECT_TRUE(std::ifstream(libObj.path).good());
        if (!libCompiled || !std::ifstream(libObj.path).good()) return;

        std::string errPath =
            (dir / (stem + "_err.txt")).string();
        std::fflush(stderr);
        std::cerr.flush();
        int saved = ::dup(STDERR_FILENO);
        int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) ::dup2(fd, STDERR_FILENO);
        {
            bezel::Driver driver;
            std::vector<bezel::Dependency> deps;
            bezel::Dependency d;
            d.metadataPath = libObj.path;
            d.objectPath = libObj.path;
            deps.push_back(d);
            appCompiled = bezel::test::compileFileForTest(
                              driver, appSrc.path, appExe.path, deps)
                              .success;
        }
        std::fflush(stderr);
        std::cerr.flush();
        if (fd >= 0) {
            ::close(fd);
            ::dup2(saved, STDERR_FILENO);
        }
        ::close(saved);
        std::ifstream in(errPath);
        err.assign((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
        std::remove(errPath.c_str());
    }
};

// The §22.6 stage-3 needle 9ef7d389's block emits. Quoted from the source
// that emits it; a rename here is a behaviour flip for every reject row.
const char* const kNoApplicable6350 =
    "no type argument binds this argument to the candidate's parameter, "
    "and the two are unrelated";

// Reject AND reject for THIS reason: the import must not compile, and the
// message must be the #6334 diagnostic. An importer that failed for an
// unrelated reason (a private type, an unexported name) satisfies neither.
void expectImportRejected6350(const std::string& libSrc,
                              const std::string& appSrc) {
    XMod6350 x(libSrc, appSrc);
    BEZEL_EXPECT_FALSE(x.appCompiled);
    BEZEL_EXPECT_TRUE(x.err.find(kNoApplicable6350) != std::string::npos);
}

// Accept AND run: the importer links against the artifact and the library's
// body executes. `expectedExit` is what the importer's `Main` RETURNS, and it
// is computed from the LIBRARY's body plus a value the importer supplied, so
// the row cannot pass on a zero, on a mis-dispatch, or on a program that never
// ran. `runProgramForTest` leaves stdout inherited (see
// `driver_run_support.hpp`), so the exit code is the only channel that reaches
// the harness — and every expected value here is well under 255, so the 8-bit
// truncation cannot alias it.
void expectImportRuns6350(const std::string& libSrc, const std::string& appSrc,
                          int expectedExit) {
    XMod6350 x(libSrc, appSrc);
    BEZEL_EXPECT_TRUE(x.appCompiled);
    if (!x.appCompiled) return;
    BEZEL_EXPECT_TRUE(std::ifstream(x.appExe.path).good());
    if (!std::ifstream(x.appExe.path).good()) return;

    int status = bezel::test::runProgramForTest(x.appExe.path);
    // `runProgramForTest` returns -1 for "did not run at all" (toolchain,
    // compile or link failure), which must not be read as an exit code.
    BEZEL_EXPECT_TRUE(status != -1);
    if (status == -1) return;
    BEZEL_EXPECT_TRUE(WIFEXITED(status));
    if (!WIFEXITED(status)) return;
    BEZEL_EXPECT_EQ(WEXITSTATUS(status), expectedExit);
}

// The library for R1/R2/R4. `Box<T>` is the issue's repro shape: a user
// generic class whose single field sits where an `Array<int>`'s own layout
// puts something else, so a mis-dispatched call reads a wrong slot rather
// than merely computing a wrong number. `U.S` returns a constant so the row
// reads `U.S(b) + b.V` — a value the CALL must have supplied.
const char* const kLibA6350 =
    "module LibA6350;\n"
    "public class Box<T>\n"
    "  public V: T := default(T);\n"
    "end Box;\n"
    "public class U\n"
    "  public static method S<V>(x: Box<V>): int => 77;\n"
    "  public method M<V>(x: Box<V>): int => 88;\n"
    "end U;\n";

// R1 — THE ISSUE'S REPRO, CROSSED. `U.S(a)` with `a: Array<int>` against a
// lone `S<V>(x: Box<V>)` that lives in ANOTHER MODULE. Inference binds
// nothing (`Array<int>` is no `Box<V>` for any V), so §14.5 drops the
// candidate and §22.6 stage 3 makes the call an error. This is the row that
// fails on 62ccf044: there the block was gated on an `ast::MethodDecl` the
// importer never has, so the §22.6 stage-3 diagnostic is never emitted.
// (An earlier version of this comment claimed the call COMPILED and SEGV'd
// under ASan; that was measured on a PRE-REBASE tree. Reviewer r31-rv-6350
// re-measured at the merge target: `ExprTypecheck.cpp:7784`'s §5.1 check
// rejects the call in BOTH states, so #6334 adds a second diagnostic rather
// than flipping an acceptance. This row is needle-discriminating; its
// `!appCompiled` guard is a precondition that never discriminates.)
BEZEL_TEST(imported_lone_generic_candidate_6334,
           imported_lone_candidate_box_v_rejects_array_spec_22_6_14_5) {
    expectImportRejected6350(
        kLibA6350,
        "module AppRepro6350; import LibA6350;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var a := new Array<int>();\n"
        "    Console.WriteLine(U.S(a));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
}

// R2 — the INSTANCE spelling across the boundary. §22.6 collects the
// candidate set from the receiver's class hierarchy, so a check written only
// on the static form would leave this one open — the same reason
// `lone_candidate_unrelated_argument_6334.
// instance_receiver_lone_generic_rejects_array_spec_22_6_12_1` exists on the
// single-module side. Constructed across the boundary, not assumed.
BEZEL_TEST(imported_lone_generic_candidate_6334,
           imported_instance_receiver_rejects_array_spec_22_6_12_1) {
    expectImportRejected6350(
        kLibA6350,
        "module AppInst6350; import LibA6350;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var u := new U();\n"
        "    using var a := new Array<int>();\n"
        "    Console.WriteLine(u.M(a));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
}

// R3 — the check is a DECISION PROCEDURE, not a `Box`-shaped special case.
// `Base<V>` is an unrelated user-declared generic in the library; `Array<int>`
// is neither a `Base<V>` for any V nor a base of one, so the same §22.6
// stage-3 rejection fires on a different pair. Single-module twin:
// `lone_candidate_unrelated_argument_6334.
// lone_generic_against_unrelated_user_class_rejects_array_spec_5_1`.
BEZEL_TEST(imported_lone_generic_candidate_6334,
           imported_unrelated_user_class_rejects_array_spec_5_1_22_6) {
    expectImportRejected6350(
        "module LibB6350;\n"
        "public class Base<T>\n"
        "end Base;\n"
        "public class W\n"
        "  public static method T<V>(x: Base<V>): int => 55;\n"
        "end W;\n",
        "module AppBase6350; import LibB6350;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var a := new Array<int>();\n"
        "    Console.WriteLine(W.T(a));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
}

// R4 — OVER-REJECT CONTROL, and the row that makes R1 a statement about
// APPLICABILITY rather than about imports. The same lone `S<V>(x: Box<V>)`,
// in the same imported library, given an actual `Box<int>`: §14.5 binds
// V=int, the substituted row admits the argument, and the call must link and
// run. The value printed is 77 (the library's body) PLUS 41 (a value this
// module stored in the box), so a zero, a wrong dispatch, or a program that
// never ran cannot produce it.
BEZEL_TEST(imported_lone_generic_candidate_6334,
           imported_lone_candidate_box_v_still_applies_to_box_int_spec_22_6) {
    // 77 (the LIBRARY's body) + 41 (a value THIS module stored in the box).
    expectImportRuns6350(
        kLibA6350,
        "module AppId6350; import LibA6350;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var b := new Box<int>();\n"
        "    b.V := 41;\n"
        "    return U.S(b) + b.V;\n"
        "  end Main;\n"
        "end Program;\n",
        118);
}

// R5 — §12.9 ACROSS THE BOUNDARY, and an HONEST NON-HINGE (read this before
// treating R5 as a fail-without row).
//
// The shape: the library exports a covariant `interface ICov<out T>` and a
// lone generic `Sink.Take<T>(x: ICov<T>)`; the importer passes an
// `Impl: ICov<int>`, so the argument reaches the parameter by §12.1 through a
// VARIANT interface and the call must run the library's body. It is the
// cross-boundary statement of §12.9's carve-out: `isSubtype` over
// declarations does not model variance, so a variance-marked parameter head
// must be left alone rather than decided.
//
// MEASURED, AND IT IS NOT A HINGE. Three separate src mutations were built and
// run against this row, and it passed all three:
//
//   (a) revert the whole #6334 block to 62ccf044 — passes, because the
//       `loneMd` gate skips the block entirely across the boundary, so the
//       block cannot over-reject there either;
//   (b) revert ONLY `sema::hasVariantGenericParam` to 62ccf044's hand-rolled
//       `pt->declaration->genericParams` walk — passes;
//   (c) widen `userClassHead` to admit an Interface head — passes.
//
// The reason (b) and (c) cannot bite is that the variance gate sits BEHIND
// `userClassHead`, which admits only a Class or Record head, and §12.9
// FORBIDS a variance marker on a class or record type parameter
// (`checkVarianceMarkerScopeForType`; MEASURED: `class CBox<out T>` is
// rejected with "variance markers are valid only on an interface's own type
// parameters (§12.9), not on a class type parameter", and likewise for
// `record`). So at this site the gate's premise cannot hold. A sentinel
// planted at the gate and swept over 489 hits (this row plus the single-module
// `generic_` / `overload_` / `module_` filters) printed `variant=1` ZERO
// times.
//
// This row is kept as a §12.9 BOUNDARY CONTROL, not as a fail-without: it
// pins that the check does not over-reject a variant interface parameter
// across the boundary, which is a real behaviour a future widening could
// break. It is NOT evidence for the accessor change. If a row is wanted that
// pins the variance read itself, it has to be one whose parameter head is a
// Class/Record carrying a variance marker — which §12.9 currently forbids, so
// such a row cannot be written today. See the handback: this is a finding
// about 9ef7d389 change 3, not a gap these rows could close.
BEZEL_TEST(imported_lone_generic_candidate_6334,
           imported_variant_interface_parameter_still_applies_spec_12_9_22_6) {
    expectImportRuns6350(
        "module LibC6350;\n"
        "public interface ICov<out T>\n"
        "end ICov;\n"
        "public class Sink\n"
        "  public static method Take<T>(x: ICov<T>): int => 7;\n"
        "end Sink;\n",
        "module AppVar6350; import LibC6350;\n"
        "public class Impl: ICov<int>\n"
        "end Impl;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var im := new Impl();\n"
        "    return Sink.Take(im) + 1;\n"
        "  end Main;\n"
        "end Program;\n",
        8);
}

}  // namespace
}  // namespace bezel::module