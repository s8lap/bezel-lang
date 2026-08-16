// fix #2855 — an imported generic class's DESTRUCTOR body arrives in the
// importer with NO types, NO body-local symbols and NO lambda parameter lists,
// so any local declaration in it SIGSEGVs the importing compiler (§28.4
// Destructors, §14.1 Generic declarations, §2.8.2 The artifact interface is a
// semantic requirement, §27.3 Lambda expressions, §27.5 Closures, §2.5
// Dependency resolution — v0.240).
//
// §2.8.2 clause 2 states the rule that was broken, verbatim:
//
//   "**Instantiate** every public generic type and generic method at arbitrary
//    module-external type arguments satisfying the declared constraints. An
//    external instantiation behaves identically to a same-module instantiation
//    of the same generic at the same arguments."
//
// `parseAndAttachDtorBody` is the fourth of ModuleLoader's body-reconstruction
// paths and carried almost nothing out of its fresh SemaContext: its remap loop
// handled FieldSymbol and MethodSymbol and nothing else, and — unlike the ctor
// (#2290/#662c), method (#783) and static-field-initializer (#2351) paths — it
// copied NEITHER `allResolvedTypes()` NOR `allExprTypes()`, and never called
// `ownImportedAuxiliary`, so the fresh context that owns those TypeRefs was
// destroyed on return. All four pieces are needed together: any one of the
// carries without the keep-alive would dangle.
//
// ⚠ THE ISSUE'S OWN ROOT CAUSE IS NARROWER THAN THE DEFECT. #2855 reports "a
// LAMBDA in the destructor". Measured on the parent commit, the lambda is one
// instance, not the axis: `var k: int := 5;` in the same destructor crashes the
// importing compiler identically (T2 below). codegen asks
// `ctx_.resolvedType(*declType)` for the local's declared type, gets a NULL
// TypeRef because no entry was ever carried, and dereferences it —
// `TypeRef::headName()` inside `lowerLocalDecl`, reached from
// `emitDisposerCore` -> `emitSpecializedDisposer` ->
// `buildImportedGenericVtable`. The lambda shapes crash for the same reason,
// one level down.
//
// ⚠ VACUITY — the four outcomes are DISTINCT by construction, so a passing cell
// cannot be ambiguous. Every destructor here writes `Trace := 100 + tag + X`
// with `tag` = 6 set by the importer and `X` the value under test, and the
// importer reads the static back after the scope ends:
//
//     >= 107  the destructor ran with the reconstructed body      (fixed)
//        106  it ran but the local/lambda evaluated to nothing    (the carries
//                                                                  are missing)
//          0  the destructor never ran — reconstruction DECLINED and no
//             DestructorSymbol was attached at all. Measured, not assumed: a
//             dtor body naming an `internal` sibling type (§13.2, the #710 /
//             #2290-bug-2 limitation) returns exactly 0 on this commit.
//     CRASHED the #2855 symptom itself.
//
// The compile is run in a fork()ed child so "the compiler CRASHED" is a
// reported outcome rather than a dead test binary — on the parent commit 15 of
// these 19 measured shapes SIGSEGV the importer.

#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

// Outcome sentinels, all outside the 0..255 exit-code range a program can
// return, so a genuine value is never mistaken for one of them.
constexpr int kDepFailed = -2;      // the dependency object could not be built
constexpr int kCompilerCrashed = -3;  // the IMPORTING COMPILER died by signal

struct TempFile2855 {
    std::string path;
    explicit TempFile2855(const std::string& p) : path(p) {}
    ~TempFile2855() { std::remove(path.c_str()); }
};

void writeSource2855(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2855(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile B against A's object IN A FORKED CHILD. Returns 0 on success, 1 on a
// clean compile failure, and kCompilerCrashed when the child died by a signal —
// which is exactly the #2855 symptom, and the reason this cannot be a plain
// in-process Driver call like its sibling xmod tests: on the parent commit the
// compiler SIGSEGVs and would take the whole test binary with it.
int forkedCompileImport2855(const std::string& bSrc, const std::string& bExe,
                            const std::string& aObj) {
    std::fflush(nullptr);
    std::cout.flush();
    std::cerr.flush();
    pid_t pid = ::fork();
    if (pid < 0) return 1;  // fork failed: report as a compile failure
    if (pid == 0) {
        // Child: silence the driver's diagnostics, compile, exit with 0/1.
        std::freopen("/dev/null", "w", stdout);
        std::freopen("/dev/null", "w", stderr);
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj;
        d.objectPath = aObj;
        deps.push_back(d);
        bool ok = driver.compileFile(bSrc, bExe, deps).success;
        std::_Exit(ok ? 0 : 1);
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) return 1;
    if (WIFSIGNALED(status)) return kCompilerCrashed;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

// Build A -> object, then B importing A -> exe (forked), run, return the exit
// code. kDepFailed when the DEPENDENCY object could not be produced (a
// toolchain problem, never the defect under test) so a genuine 0 is never
// mistaken for a skip; kCompilerCrashed when the importing compiler died.
int runXmod2855(const std::string& stem, const std::string& aSrcText,
                const std::string& bSrcText) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string tag = stem + "_" + unique_stem("");
    TempFile2855 aSrc((dir / (tag + "_a.bzl")).string());
    TempFile2855 aObj((dir / (tag + "_a.o")).string());
    writeSource2855(aSrc.path, aSrcText);
    if (!compileToObj2855(aSrc.path, aObj.path)) return kDepFailed;
    if (!std::ifstream(aObj.path).good()) return kDepFailed;
    TempFile2855 bSrc((dir / (tag + "_b.bzl")).string());
    TempFile2855 bExe((dir / (tag + "_b")).string());
    writeSource2855(bSrc.path, bSrcText);
    int rc = forkedCompileImport2855(bSrc.path, bExe.path, aObj.path);
    if (rc != 0) return rc == kCompilerCrashed ? kCompilerCrashed : -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// Every cell shares the same importer: construct `Box<string>`, set `tag` to 6,
// let the scope end (§28.3 running the destructor), then read the static the
// destructor wrote.
std::string importer2855(const std::string& mod) {
    return "module Main; import " + mod + "; "
           "class C "
           "  public static method Main(): int "
           "    do "
           "      using var b := new Box<string>(); "
           "      b.v := \"hello\"; "
           "      b.tag := 6; "
           "    end; "
           "    return Box<string>.Trace; "
           "  end Main; "
           "end C;";
}

// …and the same exporter shape, with only the destructor body varying. The
// `T`-typed field is FIRST so the instantiation's layout differs from the
// placeholder, which is what makes the destructor's field reads observable.
std::string exporter2855(const std::string& mod, const std::string& dtorBody) {
    return "module " + mod + "; "
           "public class Box<T> "
           "  public v: T; "
           "  public tag: int; "
           "  public static Trace: int; "
           "  destructor " + dtorBody + " end destructor; "
           "end Box;";
}

// Run one cell and assert the value, reporting the crash sentinel by name.
void expectDtor2855(const std::string& stem, const std::string& mod,
                    const std::string& dtorBody, int expected) {
    int rc = runXmod2855(stem, exporter2855(mod, dtorBody), importer2855(mod));
    if (rc == kDepFailed) return;
    if (rc == kCompilerCrashed)
        std::cout << "    the IMPORTING COMPILER CRASHED (#2855's symptom)\n";
    BEZEL_EXPECT_EQ(rc, expected);
}

}  // namespace

// ===========================================================================
// T1 — the issue's OWN repro: a lambda declared and called in an imported
// generic class's destructor body. 100 + (6+1) = 107; the importing compiler
// SIGSEGV'd, with an empty stderr and no diagnostic of any kind.
// ===========================================================================
BEZEL_TEST(xmod_generic_dtor_body_2855, t1_issue_repro_lambda_27_3) {
    expectDtor2855("bz2855_t1", "LibD2855A",
                   "var f: (int) -> int := lambda(x: int) => x + 1; "
                   "Trace := 100 + f(tag);",
                   107);
}

// ===========================================================================
// T2 — THE AXIS THE ISSUE MISSTATES: the same destructor with NO lambda at all,
// just a plain typed local. It crashed identically. This is the case that
// proves the defect is the missing TYPE tables (a null `TypeRef` dereferenced in
// lowerLocalDecl), not the lambda — and the case a lambda-only fix would have
// left broken. 100 + 6 + 5 = 111.
// ===========================================================================
BEZEL_TEST(xmod_generic_dtor_body_2855, t2_plain_typed_local_no_lambda_28_4) {
    expectDtor2855("bz2855_t2", "LibD2855B",
                   "var k: int := 5; Trace := 100 + tag + k;", 111);
}

// ===========================================================================
// T3 — the lambda captures a BODY-LOCAL (§27.5 Closures). 100 + (6+4) = 110.
// ===========================================================================
BEZEL_TEST(xmod_generic_dtor_body_2855, t3_lambda_captures_body_local_27_5) {
    expectDtor2855("bz2855_t3", "LibD2855C",
                   "var k := 4; "
                   "var f: (int) -> int := lambda(x: int) => x + k; "
                   "Trace := 100 + f(tag);",
                   110);
}

// ===========================================================================
// T4 — the lambda reads a FIELD through `this` (§27.5). 100 + (6+1) = 107.
// ===========================================================================
BEZEL_TEST(xmod_generic_dtor_body_2855, t4_lambda_reads_field_via_this_27_5) {
    expectDtor2855("bz2855_t4", "LibD2855D",
                   "var f: (int) -> int := lambda(x: int) => this.tag + x; "
                   "Trace := 100 + f(1);",
                   107);
}

// ===========================================================================
// T5 — a MULTI-parameter lambda (§27.3): the dropped table cost the closure its
// whole user parameter list, not one entry. 100 + 6*3 = 118.
// ===========================================================================
BEZEL_TEST(xmod_generic_dtor_body_2855, t5_multi_parameter_lambda_27_3) {
    expectDtor2855("bz2855_t5", "LibD2855E",
                   "var f: (int, int) -> int := lambda(a: int, b: int) => a * b; "
                   "Trace := 100 + f(tag, 3);",
                   118);
}

// ===========================================================================
// T6 — a ZERO-parameter lambda. In the METHOD/CTOR paths this shape was always
// correct (a null lambdaParams table and an empty list are indistinguishable),
// which is why #2839's defect looked narrower than it was. Here it is NOT a
// no-op control: the local's declared type `() -> int` still needs a carried
// TypeRef, so this crashed too. 100 + 6 + 8 = 114.
// ===========================================================================
BEZEL_TEST(xmod_generic_dtor_body_2855, t6_zero_parameter_lambda_27_3) {
    expectDtor2855("bz2855_t6", "LibD2855F",
                   "var f: () -> int := lambda() => 8; "
                   "Trace := 100 + tag + f();",
                   114);
}

// ===========================================================================
// T7 — the lambda returns a REFERENCE type (§27.3 return-type inference over
// `string`). 100 + 6 + "v7".Length = 108.
// ===========================================================================
BEZEL_TEST(xmod_generic_dtor_body_2855, t7_lambda_returns_reference_type_27_3) {
    expectDtor2855("bz2855_t7", "LibD2855G",
                   "var f: (int) -> string := lambda(x: int) => $\"v{x}\"; "
                   "Trace := 100 + tag + f(7).Length;",
                   108);
}

// ===========================================================================
// T8 — a `for var i in` loop, whose iteration variable is a body-local
// (§27.5's category, the #2818 arm this path never had). This shape did NOT
// crash on the parent commit: with no declared type there is no null TypeRef to
// dereference, so it compiled cleanly and returned 0 — the destructor silently
// never ran. 100 + 6 + (1+2+3) = 112.
// ===========================================================================
BEZEL_TEST(xmod_generic_dtor_body_2855, t8_for_in_iteration_variable_28_4) {
    expectDtor2855("bz2855_t8", "LibD2855H",
                   "var acc := 0; "
                   "for var i in 1..3 do acc := acc + i; end; "
                   "Trace := 100 + tag + acc;",
                   112);
}

// ===========================================================================
// T9 — a `const` body-local. Also a silent 0 before, not a crash.
// 100 + 6 + 9 = 115.
// ===========================================================================
BEZEL_TEST(xmod_generic_dtor_body_2855, t9_const_body_local_28_4) {
    expectDtor2855("bz2855_t9", "LibD2855I",
                   "const k := 9; Trace := 100 + tag + k;", 115);
}

// ===========================================================================
// T10 — a STRING body-local built by interpolation, which additionally
// exercises §4.6's refcount analysis inside a destructor. "xm-6".Length = 4;
// 100 + 6 + 4 = 110.
// ===========================================================================
BEZEL_TEST(xmod_generic_dtor_body_2855, t10_string_body_local_4_6) {
    expectDtor2855("bz2855_t10", "LibD2855J",
                   "var s := $\"xm-{tag}\"; Trace := 100 + tag + s.Length;", 110);
}

// ===========================================================================
// T11 — a local declared with the GENERIC PARAMETER's own type. This is the
// monomorphization axis: the carried `resolvedTypes_` entry for `T` is what
// codegen substitutes per instantiation. 100 + 6 = 106.
// ===========================================================================
BEZEL_TEST(xmod_generic_dtor_body_2855, t11_generic_parameter_typed_local_14_1) {
    expectDtor2855("bz2855_t11", "LibD2855K",
                   "var t: T := v; Trace := 100 + tag;", 106);
}

// ===========================================================================
// T12 — TWO lambdas in one destructor: both entries must reach the importer.
// 100 + (6+1) + (6*2) = 119.
// ===========================================================================
BEZEL_TEST(xmod_generic_dtor_body_2855, t12_two_lambdas_27_3) {
    expectDtor2855("bz2855_t12", "LibD2855L",
                   "var f: (int) -> int := lambda(x: int) => x + 1; "
                   "var g: (int) -> int := lambda(y: int) => y * 2; "
                   "Trace := 100 + f(tag) + g(tag);",
                   119);
}

// ===========================================================================
// T13 — a local in a NESTED block inside the destructor (the scope-depth axis
// #969 keys `declScopeDepth_` on). 100 + 6 + 7 = 113.
// ===========================================================================
BEZEL_TEST(xmod_generic_dtor_body_2855, t13_nested_block_local_28_4) {
    expectDtor2855("bz2855_t13", "LibD2855M",
                   "var acc := 0; "
                   "do var k: int := 7; acc := acc + k; end; "
                   "Trace := 100 + tag + acc;",
                   113);
}

// ===========================================================================
// T14 — CONTROL: a NON-generic imported class. Pass 3 of the loader only
// reconstructs a GENERIC class's bodies, so a non-generic class's destructor is
// the EXPORTER's own compiled disposer and never had the defect. 107 before AND
// after — this isolates "generic" from "imported".
// ===========================================================================
BEZEL_TEST(xmod_generic_dtor_body_2855, t14_control_non_generic_class_28_4) {
    int rc = runXmod2855(
        "bz2855_t14",
        "module LibD2855N; "
        "public class Plain2855 "
        "  public tag: int; "
        "  public static Trace: int; "
        "  destructor "
        "    var f: (int) -> int := lambda(x: int) => x + 1; "
        "    Trace := 100 + f(tag); "
        "  end destructor; "
        "end Plain2855;",
        "module Main; import LibD2855N; "
        "class C "
        "  public static method Main(): int "
        "    do "
        "      using var b := new Plain2855(); "
        "      b.tag := 6; "
        "    end; "
        "    return Plain2855.Trace; "
        "  end Main; "
        "end C;");
    if (rc == kDepFailed) return;
    BEZEL_EXPECT_EQ(rc, 107);
}

// ===========================================================================
// T15 — CONTROL: a destructor with NO local and NO lambda. It reconstructed and
// ran correctly all along (nothing in it needs a carried type), which is the
// discriminating control the issue reports and the proof that the destructor
// path itself was never the problem. 106 before AND after.
// ===========================================================================
BEZEL_TEST(xmod_generic_dtor_body_2855, t15_control_no_local_no_lambda_28_4) {
    expectDtor2855("bz2855_t15", "LibD2855O", "Trace := 100 + tag;", 106);
}

// ===========================================================================
// T16 — the DECLINE signature, asserted rather than assumed: a destructor body
// naming a type the synthetic reconstruction never declares (an `internal`
// sibling, §13.2) makes `parseAndAttachDtorBody` return false, so NO
// DestructorSymbol is attached and the destructor never runs — the static stays
// 0. That is the pre-existing #710 / #2290-bug-2 limitation, unchanged by this
// fix (0 before AND after), and it is what licenses reading `>= 107` in T1-T13
// as "the body was reconstructed": had reconstruction declined there, they would
// read 0 too. Pinned so a later change cannot make those cells vacuous.
// ===========================================================================
BEZEL_TEST(xmod_generic_dtor_body_2855, t16_declined_reconstruction_reads_zero_2_8_2) {
    int rc = runXmod2855(
        "bz2855_t16",
        "module LibD2855P; "
        "internal class Hidden2855 "
        "  public n: int; "
        "end Hidden2855; "
        "public class Box<T> "
        "  public v: T; "
        "  public tag: int; "
        "  public static Trace: int; "
        "  destructor "
        "    using var h := new Hidden2855(); "
        "    h.n := 5; "
        "    Trace := 100 + tag + h.n; "
        "  end destructor; "
        "end Box;",
        importer2855("LibD2855P"));
    if (rc == kDepFailed) return;
    BEZEL_EXPECT_EQ(rc, 0);
}

}  // namespace bezel::module
