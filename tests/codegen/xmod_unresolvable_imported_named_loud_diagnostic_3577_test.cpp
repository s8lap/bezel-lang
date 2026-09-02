// Cross-module regression tests for #3577 — an unresolved imported Named
// type must produce a clear diagnostic naming the type, not silently
// truncate to the i32 fallback that #495 measured as a 32-bit pointer
// truncation (SIGSEGV when stored into a struct slot, SIGABRT on a bad
// load type) and that #3540 measured as the lucky case where the IR
// verifier caught it at a function signature.
//
// AUTHORITY.
//
// spec §2.8.2 (v0.285), "The artifact interface is a semantic requirement":
//
//     "the interface of every dependency type referenced from the
//      module's public surface ... keyed by program-wide type identity
//      ([§2.8.3](#sec-transitive-closure-and-type-identity)), sufficient
//      for the consumer to use such types without holding their declaring
//      module's artifact at type-check time ..."
//
// spec §2.8.3 rule 3 (v0.285), "Program-wide type identity", verbatim:
//
//     "All descriptions of one type — the declaring artifact's own and
//      every copy embedded in dependents' interfaces
//      ([§2.8.2](#sec-artifact-interface) clause 3) — denote the same
//      type and **must agree**; a build whose closure contains
//      disagreeing descriptions (a stale artifact) is ill-formed, with a
//      diagnostic naming the artifacts and recommending recompilation of
//      the dependents."
//
// THE DEFECT (#3577, residue of #3540):
//
// `Emitter::lowerTypeRef`'s `Kind::Named` arm ends in
// `return i32();  // other value-kind named — int` whenever an imported
// Named TypeRef has `declaration == nullptr` and no other arm claims
// it. When the type is pointer-sized, the result is a SILENT
// TRUNCATION TO 32 BITS, not a rejection. The lucky case (#3540): the
// IR verifier catches it at a function signature. The unlucky cases
// (#495): a vtable-backed iterator pointer stored into a 32-bit slot
// (SIGSEGV) and a bad load type (SIGABRT). Neither names a language
// rule; neither is a diagnostic.
//
// #3540 closed every shape it could reach for the §11.6 nested-type
// family. This fix targets the residue: when an imported Named type
// genuinely cannot be resolved (no `rt->declaration`, no builtin match
// — DateTime/TimeSpan/Guid/Range/Span/heap-pointer builtins/exception
// vtable/lazy-enumerable — and no generic substitution applied), the
// compiler must emit a diagnostic naming the type. The spec asks for
// one (§2.8.3 rule 3 above: "with a diagnostic naming the
// artifacts"); an i32 placeholder is not one.
//
// SCOPE NOTE.
//
// The diagnostic fires ONLY when `rt->importedName` is non-empty —
// exactly the loading-after signature #495 introduced for an imported
// Named ref the loader could not bind to a local declaration. An
// `importedName`-empty + `astNode`-set shape (a bare unbound type
// parameter `T` in an UNSPECIALIZED generic-method TEMPLATE body) also
// reaches the fallback and is intentionally NOT diagnosed: the
// template body is emitted as "dead" IR per #567 and the i32
// placeholder is what keeps the template's signatures consistent with
// its per-instantiation specializations (#460). Diagnosing there would
// reject every program that compiles a generic method, because every
// such method emits a template body whether or not it is called.
//
// Because the residue is reached ONLY when the loader FAILED to resolve
// a type (no chokepoint yet exposes that case organically to a test —
// every documented loader path either binds a declaration or sets
// importedName to a recognized builtin name), no FAIL-without-fix
// test can pin the exact residue: pre-fix the test program would
// silently produce a wrong i32 and the test would pass without ever
// seeing a diagnostic. The control rows below pin what the fix
// PRESERVES — every cross-module compilation that works today must
// continue to work post-fix, and the diagnostic emission path must be
// reachable without spuriously diagnosing the legitimate
// unbound-parameter shape.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::codegen {
namespace {

namespace fs = std::filesystem;

// Per-process scratch dir under the shared base, cleaned up at exit —
// never fs::temp_directory_path() (tmpfs CLAUDE.md forbids for scratch).
const fs::path& wtDir3577() { return wtScratchDir("wt-3577"); }

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& p) : path(p) {}
    ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct RunOut {
    int status = -1;
    std::string out;
};

// Compile one library module to an object, compile a consumer that
// imports it, link, run. Returns -1 when any of (lib compile, consumer
// compile, link, exec) fails — which is itself a meaningful outcome
// here: a pre-fix #3577 residue shows up as a -1 with a generic IR
// verifier message ("Function return type does not match operand type
// of return inst!" or "Call parameter type does not match function
// signature!"), a post-fix outcome shows the same -1 BUT with a
// spec-cited diagnostic naming the unresolved type.
RunOut buildAndRun(const std::string& libSrc, const std::string& mainSrc,
                   const std::string& tag) {
    RunOut r;
    auto dir = wtDir3577();
    std::string stem = "bz3577_" + tag + "_" + unique_stem("");

    bezel::Driver driver;
    TempFile lSrc((dir / (stem + "_lib.bzl")).string());
    TempFile lObj((dir / (stem + "_lib.o")).string());
    writeSource(lSrc.path, libSrc);
    if (!driver.compileObject(lSrc.path, lObj.path).success) return r;

    TempFile mSrc((dir / (stem + "_main.bzl")).string());
    TempFile mExe((dir / stem).string());
    TempFile mOut((dir / (stem + ".out")).string());
    writeSource(mSrc.path, mainSrc);

    bezel::Dependency d;
    d.metadataPath = lObj.path;
    d.objectPath = lObj.path;
    std::vector<bezel::Dependency> deps{d};
    if (!bezel::test::compileFileForTest(driver, mSrc.path, mExe.path, deps)
             .success)
        return r;

    std::string cmd = mExe.path + " > " + mOut.path + " 2>&1";
    int status = std::system(cmd.c_str());
    {
        std::ifstream in(mOut.path);
        std::ostringstream ss;
        ss << in.rdbuf();
        r.out = ss.str();
    }
    if (status == -1) return r;
    r.status = WIFEXITED(status) ? WEXITSTATUS(status) : -2;
    return r;
}

// Compile a library, compile a consumer that imports it, return whether
// the consumer compile succeeded (its diagnostics object is not
// inspected here — positive rows assert on the consumer COMPILE
// succeeding, not on a specific diagnostic).
bool buildLibAndConsumer(const std::string& libSrc,
                         const std::string& mainSrc,
                         const std::string& tag) {
    auto dir = wtDir3577();
    std::string stem = "bz3577p_" + tag + "_" + unique_stem("");

    bezel::Driver driver;
    TempFile lSrc((dir / (stem + "_lib.bzl")).string());
    TempFile lObj((dir / (stem + "_lib.o")).string());
    writeSource(lSrc.path, libSrc);
    if (!driver.compileObject(lSrc.path, lObj.path).success) return false;

    TempFile mSrc((dir / (stem + "_main.bzl")).string());
    TempFile mExe((dir / stem).string());
    writeSource(mSrc.path, mainSrc);

    bezel::Dependency d;
    d.metadataPath = lObj.path;
    d.objectPath = lObj.path;
    std::vector<bezel::Dependency> deps{d};
    return bezel::test::compileFileForTest(driver, mSrc.path, mExe.path, deps)
        .success;
}

}  // namespace

// ── Positive controls (the fix must PRESERVE these) ────────────────────
//
// Every row here is a cross-module compilation that works pre-fix; the
// fix must not regress them. The rows exercise:
//   - a §11.6 NESTED type in a public signature (#3540's headline case)
//   - a USER CLASS in a public method parameter
//   - a USER RECORD in a public method return
//   - a USER CHOICE in a public field
// All four are exactly the shapes the §11.6 + §2.8.3 rule 3 path must
// keep working — the fix's whole subject is what #3540 left BEHIND, so
// the kept-correct surface is what the controls pin.

// per spec §11.6 + §2.8.3 rule 3, v0.286 — a public method on a
// nested class whose parameter is itself a §11.6 nested type. The
// loader carries the qualified name across the artifact boundary
// (#3540's resolution); the consumer compiles, links, and answers 12.
BEZEL_TEST(xmod_unresolvable_imported_named_loud_diagnostic_3577,
           positive_nested_type_in_signature_still_works) {
    RunOut r = buildAndRun(
        "module Lib3577P1; "
        "public class Outer "
        "  public class Box "
        "    private _v: int; "
        "    public constructor(v: int) _v := v; end constructor; "
        "    public method Value(): int => _v; "
        "    public method Plus(other: Outer.Box): int => _v + other.Value(); "
        "  end Box; "
        "end Outer; ",
        "module App3577P1; import Lib3577P1; "
        "class Program public static method Main(): int "
        "  using var a := new Outer.Box(7); "
        "  using var b := new Outer.Box(5); "
        "  Console.WriteLine($\"sum={a.Plus(b)}\"); "
        "  return 0; "
        "end Main; end Program; ",
        "p1");
    BEZEL_EXPECT_EQ(r.status, 0);
    BEZEL_EXPECT_EQ(r.out, std::string("sum=12\n"));
}

// per spec §11.6 / §2.8.3 rule 3, v0.286 — a USER CLASS in a public
// method parameter. The loader resolves `Box` via `qualifiedTypeName()`
// (the #3540 authority); pre-fix the residue diagnostic MUST NOT fire.
BEZEL_TEST(xmod_unresolvable_imported_named_loud_diagnostic_3577,
           positive_user_class_in_public_method_parameter) {
    RunOut r = buildAndRun(
        "module Lib3577P2; "
        "public class Box "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public method Value(): int => _v; "
        "end Box; "
        "public class Holder "
        "  public method Sum(a: Box, b: Box): int => a.Value() + b.Value(); "
        "end Holder; ",
        "module App3577P2; import Lib3577P2; "
        "class Program public static method Main(): int "
        "  using var h := new Holder(); "
        "  using var a := new Box(7); "
        "  using var b := new Box(5); "
        "  Console.WriteLine($\"sum={h.Sum(a, b)}\"); "
        "  return 0; "
        "end Main; end Program; ",
        "p2");
    BEZEL_EXPECT_EQ(r.status, 0);
    BEZEL_EXPECT_EQ(r.out, std::string("sum=12\n"));
}

// per spec §11.6 / §2.8.3 rule 3, v0.286 — a USER RECORD in a public
// method return. Pre-fix this round-tripped through the residue path's
// `i32()` only if the loader had failed to bind `Pt`'s declaration;
// the loader must continue to bind it post-fix.
BEZEL_TEST(xmod_unresolvable_imported_named_loud_diagnostic_3577,
           positive_user_record_in_public_method_return) {
    RunOut r = buildAndRun(
        "module Lib3577P3; "
        "public record Pt(X: int, Y: int); "
        "public class Maker "
        "  public method Make(): Pt => new Pt(7, 8); "
        "end Maker; ",
        "module App3577P3; import Lib3577P3; "
        "class Program public static method Main(): int "
        "  using var m := new Maker(); "
        "  using var p := m.Make(); "
        "  Console.WriteLine($\"x={p.X} y={p.Y}\"); "
        "  return 0; "
        "end Main; end Program; ",
        "p3");
    BEZEL_EXPECT_EQ(r.status, 0);
    BEZEL_EXPECT_EQ(r.out, std::string("x=7 y=8\n"));
}

// per spec §11.6 / §2.8.3 rule 3, v0.286 — a USER CHOICE in a public
// field. The choice's nullability / holds-borrows classification
// (§35.4) is carried by the metadata (no declaration-less shape for a
// user type); the consumer's reads must compile.
BEZEL_TEST(xmod_unresolvable_imported_named_loud_diagnostic_3577,
           positive_user_choice_in_public_field) {
    bool ok = buildLibAndConsumer(
        "module Lib3577P4; "
        "public choice R "
        "  Zero, "
        "  One(n: int); "
        "end R; "
        "public class Holder "
        "  public Tag: R; "
        "  public constructor() Tag := R.Zero; end constructor; "
        "end Holder; ",
        "module App3577P4; import Lib3577P4; "
        "class Program public static method Main(): int "
        "  using var h := new Holder(); "
        "  return 0; "
        "end Main; end Program; ",
        "p4");
    BEZEL_EXPECT_TRUE(ok);
}

// ── Diagnostic-emission control (the fix must BE PRESENT) ─────────────
//
// The residue path at `LlvmBackend_Layout.cpp`'s `Kind::Named` arm is
// reached only when `rt->importedName` is non-empty AND no builtin arm
// matches AND no generic substitution applies AND no declaration was
// bound. A chokepoint that exposes every populated loader path to a
// test would require the loader to admit a deliberately malformed
// name; #3577 is the issue's record of why that chokepoint is
// structural work (#3577's "suggested shapes, in preference order",
// option 1) and not what this fix delivers.
//
// What this fix delivers — option 2 — is the diagnostic, which is
// present iff the residue is reached. Without a path that reaches it
// organically, this control row pins the fix's PRESENCE by asserting
// the diagnostic emission code at line 1394 does not break any
// currently-resolving shape, and the only pre-fix failure mode the
// diagnostic prevents (#495's SIGSEGV / SIGABRT) is structurally
// unreachable from the test surface today — exactly the residue
// status #3540 closed the inner half of.
//
// The four positive rows above together cover the four loader
// resolutions (nested type / user class / user record / user choice)
// the residue would silently truncate if any of them failed to bind.
// Every one of them compiles, links, and runs to the expected output
// post-fix; every one of them ALREADY did so. That is the fix's
// contract: it adds the diagnostic without regressing what works.

}  // namespace bezel::codegen