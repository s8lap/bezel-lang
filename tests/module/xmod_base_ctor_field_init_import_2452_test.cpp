// fix #2452 (§22.8 step 1-2 / §2.5 / §2.8.2-§2.8.3, v0.235): a LOCAL class
// deriving from an IMPORTED base never ran the base's field initializers
// (implicit default construction) or the base's own no-explicit-ctor
// construction sequence reached via an explicit `base()` chain call —
// silently wrong instance state, no diagnostic, no crash. Every inherited
// field read as its calloc-zeroed default.
//
// ROOT CAUSE: `Emitter::emitImplicitBase` (LlvmBackend_Setup.cpp) — reached
// both from `lowerNew`'s no-explicit-ctor construction sequence and from
// `emitChainCall`'s explicit `base(...)` handling when the target has no
// matching ConstructorSymbol — falls back to `emitOwnFieldInit(b, obj, …)`
// whenever base `b` has no explicit constructor. `emitOwnFieldInit` walks
// `ast::FieldDecl`/`PropertyDecl` initializer *expressions* off each
// member's `decl` pointer to know what to run — always null for an IMPORTED
// symbol, since the artifact interface ships no AST across the module
// boundary (§2.8.2/§2.8.3, the #2374-class gap ~20 other sites in this
// codebase already hit). Every field silently kept its default with no
// initializer to run.
//
// For comparison, a *direct* `new ImportedBase()` (no local derivation) was
// already correct before this fix (T5 below) — proving the exported
// `bz$<Base>$init` body itself is right; only the locally-derived
// construction paths failed to reach it.
//
// THE FIX (this commit): a new helper, `Emitter::emitImportedBaseFieldInit`,
// detects exactly this shape (`b` imported, no explicit constructor, non-
// generic) and routes through the ALREADY-EXPORTED `bz$<Base>$init` instead
// — the same entry point a direct `new ImportedBase()` already uses, so it
// runs `b`'s real field initializers (and `b`'s own base chain, recursively,
// through the SAME mechanism at that level) via the AST that exists in `b`'s
// OWN module. No new metadata field or export was added: `bz$<Type>$init` is
// already unconditionally emitted for every public non-generic class; this
// is a new CALL SITE for an existing, always-present symbol, exactly
// mirroring the established `declareImportedInit`/`declareImportedCtor`
// naming-convention pattern. Because `bz$<Base>$init` also (correctly, for
// its own direct-`new` use) installs `b`'s OWN vtable at obj[0], the helper
// saves and restores that slot around the call so it does not clobber the
// most-derived vtable the `new` site already installed — T4 below is an
// adversarial check that this save/restore does not itself break virtual
// dispatch.
//
// SCOPE (what this fix does NOT cover, filed separately):
//   - #2452's own Case B repro (`class D: Base(99)` with `D` having NO
//     primary-constructor parameters of its own) hits a DIFFERENT,
//     pre-existing parser gap: `Parser::parseInheritance` (the non-
//     positional-fields path, Parser_Decls.cpp) tokenizes away the base's
//     argument list entirely for a class without primary-ctor params,
//     regardless of whether the base is local or imported — confirmed by a
//     LOCAL-only repro showing the identical silent-zero symptom. That is a
//     broader, general defect unrelated to cross-module construction; filed
//     separately rather than fixed here (out of this issue's scope).
//   - A distinct, also-newly-found property-DISPATCH bug (not construction):
//     reading a non-virtual property inherited from an imported base through
//     a derived instance that has its OWN additional field returns the
//     OWN field's value instead of the inherited one. Filed separately — a
//     different mechanism (member-access lowering, not `new`/ctor-chain
//     codegen) hit while building this test's adversarial grid. T1-T4 below
//     use derived classes with NO own fields specifically to avoid
//     conflating the two bugs in this fix's own regression coverage.

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
#include "test_harness.hpp"

namespace bezel::module {
namespace {

struct TempFile2452 {
    std::string path;
    explicit TempFile2452(std::string p) : path(std::move(p)) {}
    ~TempFile2452() { std::remove(path.c_str()); }
};

void writeSource2452(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2452(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile lib -> object, compile app (importing lib) -> exe, run it in a
// SEPARATE process (fork-isolated per tests/README.md's AOT harness
// discipline), return its exit code (-1 on compile/link/run failure, -2 if
// the dependency itself could not be produced). Mirrors #2387/#2414/#2449's
// own compileImportAndRun helpers.
int compileImportAndRun2452(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2452_" + std::to_string(std::rand());
    TempFile2452 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2452 aObj((dir / (stem + "_a.o")).string());
    writeSource2452(aSrc.path, libSrc);
    if (!compileToObj2452(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2452 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2452 bExe((dir / stem).string());
    writeSource2452(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    if (!driver.compileFile(bSrc.path, bExe.path, deps).success) return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

}  // namespace

// ===========================================================================
// T1 — the #2452 repro's Case A, verbatim: a LOCAL class with NO explicit
// constructor deriving from an IMPORTED base whose ONLY field is initialized
// by a field initializer (`_v: int := 42`, no explicit constructor at all on
// the base either). `new Derived()` runs the implicit ctor's implicit
// `base()`, which reaches `emitImplicitBase`'s no-parameterless-ctor
// fallback for the imported base. 42 is a value that cannot arise from the
// calloc-zeroed default (0), so a passing assertion is a genuine proof the
// initializer ran, not a coincidence.
// Pre-fix: 0 (silent, no diagnostic). Post-fix: 42.
// ===========================================================================
BEZEL_TEST(xmod_base_ctor_field_init_import_2452,
           implicit_default_ctor_runs_imported_base_field_initializer_22_8) {
    int r = compileImportAndRun2452(
        "module BaseLib2452T1; "
        "public class Base2452T1 "
        "  private _v: int := 42; "
        "  public property Val: int => _v; "
        "end Base2452T1;",
        "module App2452T1; import BaseLib2452T1; "
        "public class Derived2452T1: Base2452T1 "
        "end Derived2452T1; "
        "public class C2452T1 "
        "  public static method Main(): int "
        "    using var d := new Derived2452T1(); "
        "    return d.Val; "
        "  end Main; "
        "end C2452T1;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 42);
}

// ===========================================================================
// T2 — same shape as T1, but the derived class declares its OWN explicit
// (empty) constructor with no written chain call, so the implicit `base()`
// is inserted by `emitConstructorBody` (a DIFFERENT call site into the same
// `emitImplicitBase`, LlvmBackend_Setup.cpp ~7028) rather than lowerNew's
// no-explicit-ctor branch. Proves the fix is reached uniformly regardless of
// which caller triggers the implicit base chain.
// ===========================================================================
BEZEL_TEST(xmod_base_ctor_field_init_import_2452,
           explicit_empty_ctor_implicit_base_runs_imported_field_init_22_8) {
    int r = compileImportAndRun2452(
        "module BaseLib2452T2; "
        "public class Base2452T2 "
        "  private _v: int := 42; "
        "  public property Val: int => _v; "
        "end Base2452T2;",
        "module App2452T2; import BaseLib2452T2; "
        "public class Derived2452T2: Base2452T2 "
        "  public constructor() "
        "  end constructor; "
        "end Derived2452T2; "
        "public class C2452T2 "
        "  public static method Main(): int "
        "    using var d := new Derived2452T2(); "
        "    return d.Val; "
        "  end Main; "
        "end C2452T2;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 42);
}

// ===========================================================================
// T3 — the EXPLICIT `base();` chain-call shape: the derived class's own
// constructor writes `base();` explicitly. `ctorByArity` finds no matching
// ConstructorSymbol on the imported no-ctor base, so `emitChainCall`'s own
// null-target branch (LlvmBackend_Setup.cpp ~7652) is the site reached —
// this fix's SECOND call site, found by sweeping the same defect shape
// (`emitOwnFieldInit` called directly on a possibly-imported `targetCls`)
// rather than just the issue's own repro line.
// ===========================================================================
BEZEL_TEST(xmod_base_ctor_field_init_import_2452,
           explicit_base_chain_call_runs_imported_no_ctor_field_init_22_8) {
    int r = compileImportAndRun2452(
        "module BaseLib2452T3; "
        "public class Base2452T3 "
        "  private _v: int := 42; "
        "  public property Val: int => _v; "
        "end Base2452T3;",
        "module App2452T3; import BaseLib2452T3; "
        "public class Derived2452T3: Base2452T3 "
        "  public constructor() "
        "    base(); "
        "  end constructor; "
        "end Derived2452T3; "
        "public class C2452T3 "
        "  public static method Main(): int "
        "    using var d := new Derived2452T3(); "
        "    return d.Val; "
        "  end Main; "
        "end C2452T3;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 42);
}

// ===========================================================================
// T4 — ADVERSARIAL: `bz$<Base>$init` (reused by this fix) also installs the
// BASE's own vtable at obj[0] as a side effect (correct for a *direct*
// `new ImportedBase()`, where the base IS the most-derived type). This test
// proves the fix's save/restore of that slot does NOT clobber the
// MOST-DERIVED (locally-overridden) vtable the `new` site already installed:
// the imported base declares a `virtual` method AND a field initializer; the
// local derived class overrides the method. `b.Who()` through a
// BASE-TYPED reference must still dispatch to the OVERRIDE (2), and
// `d.V` must still observe the real inherited field value (7) — encoding
// both as a single composite value (`Who()*100 + V`) so either a dispatch
// regression or a construction regression fails this one assertion.
// Pre-fix: this shape hit the SAME zero-field bug as T1 (V would read 0);
// this test's real purpose is guarding the vtable-preservation invariant
// the fix's OWN save/restore mechanism introduces.
// ===========================================================================
BEZEL_TEST(xmod_base_ctor_field_init_import_2452,
           vtable_save_restore_preserves_derived_override_22_8) {
    int r = compileImportAndRun2452(
        "module VtLib2452T4; "
        "public class VtBase2452T4 "
        "  private _v: int := 7; "
        "  public virtual method Who(): int "
        "    return 1; "
        "  end Who; "
        "  public property V: int => _v; "
        "end VtBase2452T4;",
        "module App2452T4; import VtLib2452T4; "
        "public class VtDerived2452T4: VtBase2452T4 "
        "  public override method Who(): int "
        "    return 2; "
        "  end Who; "
        "end VtDerived2452T4; "
        "public class C2452T4 "
        "  public static method Main(): int "
        "    using var d := new VtDerived2452T4(); "
        "    var b: VtBase2452T4 := d; "
        "    return b.Who() * 100 + d.V; "
        "  end Main; "
        "end C2452T4;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 207);  // 2*100 + 7
}

// ===========================================================================
// T5 — CONTROL: a *direct* `new ImportedBase()` (no local derivation at all)
// was already correct before this fix — it goes through `lowerNew`'s
// `ctx_.isImported(t) && !importedGeneric` branch (declareImportedInit),
// untouched by this change. Guards against the new
// `emitImportedBaseFieldInit` helper accidentally being reached from, or
// otherwise interfering with, that unrelated path.
// ===========================================================================
BEZEL_TEST(xmod_base_ctor_field_init_import_2452,
           direct_import_construction_unaffected_control) {
    int r = compileImportAndRun2452(
        "module BaseLib2452T5; "
        "public class Base2452T5 "
        "  private _v: int := 42; "
        "  public property Val: int => _v; "
        "end Base2452T5;",
        "module App2452T5; import BaseLib2452T5; "
        "public class C2452T5 "
        "  public static method Main(): int "
        "    using var d := new Base2452T5(); "
        "    return d.Val; "
        "  end Main; "
        "end C2452T5;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 42);
}

// ===========================================================================
// T6 — CONTROL: the SAME shape as T1, but with a fully LOCAL base (no import
// at all). Guards against the new `ctx_.isImported(b)`-gated branch in
// `emitImplicitBase` accidentally engaging (or otherwise changing behavior)
// for a local base — it must take the original `emitOwnFieldInit` path
// unchanged.
// ===========================================================================
BEZEL_TEST(xmod_base_ctor_field_init_import_2452,
           local_base_construction_unaffected_control) {
    bezel::Driver driver;
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2452_t6_" + std::to_string(std::rand());
    TempFile2452 src((dir / (stem + ".bzl")).string());
    TempFile2452 exe((dir / stem).string());
    writeSource2452(src.path,
                    "module LocalCtl2452T6; "
                    "public class LocalBase2452T6 "
                    "  private _v: int := 42; "
                    "  public property Val: int => _v; "
                    "end LocalBase2452T6; "
                    "public class LocalDerived2452T6: LocalBase2452T6 "
                    "end LocalDerived2452T6; "
                    "public class C2452T6 "
                    "  public static method Main(): int "
                    "    using var d := new LocalDerived2452T6(); "
                    "    return d.Val; "
                    "  end Main; "
                    "end C2452T6;");
    BEZEL_EXPECT_TRUE(driver.compileFile(src.path, exe.path, {}).success);
    int status = std::system(exe.path.c_str());
    int r = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(r, 42);
}

// ===========================================================================
// T7 — CONTROL: a primary-constructor LOCAL derived class chaining args to
// an IMPORTED base's EXPLICIT constructor (`class D(v: int): Base(v)`) —
// this shape ALREADY worked before this fix (a different, pre-existing
// `emitImplicitBase` branch: `td->primaryBaseCtorArgs` +
// `declareImportedCtor`, untouched here). Guards against a regression in
// that nearby, adjacent branch of the same function this fix modifies.
// ===========================================================================
BEZEL_TEST(xmod_base_ctor_field_init_import_2452,
           primary_ctor_derived_chains_args_to_imported_explicit_ctor_control) {
    int r = compileImportAndRun2452(
        "module BaseLib2452T7; "
        "public class Base2452T7 "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public property Val: int => _v; "
        "end Base2452T7;",
        "module App2452T7; import BaseLib2452T7; "
        "public class Derived2452T7(v: int): Base2452T7(v) "
        "end Derived2452T7; "
        "public class C2452T7 "
        "  public static method Main(): int "
        "    using var d := new Derived2452T7(99); "
        "    return d.Val; "
        "  end Main; "
        "end C2452T7;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 99);
}

}  // namespace bezel::module
