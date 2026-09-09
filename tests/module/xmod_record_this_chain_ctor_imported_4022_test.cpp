// fix #4022 (§22.7 / §22.8, v0.294): fix #4013/#4019 routed a record's
// leading `this(...)` chain through the shared `emitChainCall` for the
// SINGLE-MODULE case. The fix landed the AST-gated branch correctly — the
// importer, however, runs the same `buildRecordValue` code path against an
// `ast::ConstructorDecl` whose `ModuleLoader::parseAndAttachCtorBody` cannot
// always reconstruct for the importer side. Two specific failure modes hit:
//
//   1. The importer keeps BOTH defects from #4013 and #4019. #4013 (chain
//      never runs): the imported record's `this(...)` body is shipped in the
//      ctor body text (see `ModuleMetadataBuilder.cpp`), but the importer's
//      `parseAndAttachCtorBody` may FAIL to reparse it — the synthetic
//      source declares only the one type, so sibling-name references
//      (`this(new Sibling(args))`, #2290 bug 2) leave `cd->body` empty on
//      the importer side. The AST-gated `chainCallOf` predicate in
//      `buildRecordValue` then returns false (no body to look at), the chain
//      falls through to `lowerScopedBlock`, `this(...)` produces no IR, and
//      the chained-to constructor's body never runs — exactly the original
//      #4013 defect, on the importer side only.
//
//   2. #4019 (double field-init): even when `cd->body` is non-empty, the
//      chain call runs the chained-to constructor's compiled ctor
//      (with its own `!isThis`-gated `emitOwnFieldInit`), AND the importer's
//      `buildRecordValue` preamble runs the field initializers a SECOND
//      time (fix #2464's preamble was unconditional, the #4019 narrowing
//      is per-ConstructorDecl only). §22.8 named the defect precisely:
//      "assigning `readonly` fields twice", "repeating allocations and
//      initializer side effects".
//
// The fix takes the #4022 path: when the explicit ctor's `decl` is null AND
// the type is imported (`ctx_.isImported(t)`), skip the field-init preamble
// AND emit a direct call to the imported ctor's compiled function
// (`declareImportedCtor`). A SECOND fix was required: `LlvmBackend_Setup.cpp`
// must declare ALL imported ctors upfront (not lazily) so LLVM's name-suffix
// assignment for collisions (`bz$<cls>$ctor` vs `bz$<cls>$ctor.1` for two
// ctors of the same class) MATCHES between exporter and importer — without
// that, the importer's lazy declaration would land on a different name slot
// than the exporter's eager one, and the call would resolve to a function
// with the WRONG signature (e.g. a 0-arg call site landing on a `(ptr, i32)`
// function).
//
// T1 is the headline case — the SIGSEGV from #4022 — which is identical to
// the test `record_this_chain_discharges_reference_field` in
// tests/codegen/record_this_chain_ctor_4013_test.cpp except across a module
// boundary: an imported record whose zero-arg ctor chains via
// `this(new Widget4022(88))` to a one-arg ctor that assigns the reference-
// typed property `Extra`. Pre-fix on the imported side, the chain never
// runs and `Extra` is the null watermark; `c.Extra.Render()` SIGSEGVs
// (`exit=139`). T1b is the single-module twin that proves the cross-module
// shape, not a different defect, is what trips #4022.
//
// T2 is the imported class case — fix #4013's own class-side coverage in
// tests/codegen/record_this_chain_ctor_4013_test.cpp
// (`class_this_chain_control_unaffected`) was passing because
// `inlineCtorBody`'s call site was never lazy about the imported ctor's
// declaration. The IMPORTER's `emitChainCall` (inherited from
// `emitConstructorBody`'s `chainCallOf` machinery) WAS lazy — it declared
// the imported ctor on first use, getting the WRONG LLVM name slot. Pre-fix
// the importer returned 16 (a 4-byte field's previous contents, the wrong
// ctor was called — the 1-arg `ctor(ptr, i32)` instead of the 0-arg
// `ctor(ptr)`, with the 4 bytes of the call arg interpreted as the field
// return) where the spec demands 99. Post-fix it returns 99.
//
// T3 is the imported record with a non-reference-typed chain — `this(42)`
// to a one-arg ctor that sets an int property `N`. Pre-fix on the importer
// side the chain never ran (the importer's `cd->body` reparse succeeded
// here, but the AST gate still saw no chain call because the `this(...)`
// reparse fell into `lowerScopedBlock`), so `N` came back 0 instead of 42.
// This case happens to share its single-module twin with #4013's int case
// — T3b is the cross-module version. T3c is the §22.8 double-init control:
// a chain call composed with a non-idempotent field initializer (a static
// counter), where the pre-fix double-run would have left `Counter` at 2.
//
// T4 is the no-chain control: an imported record with NO `this(...)` chain
// — just a 0-arg ctor that assigns a field. Must keep working on both
// sides, and the assertion's value is its own correctness — pre- AND post-
// fix this returns 99, so a regression in this control means we broke an
// unrelated path.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"  // #3156: the ONE sanitizer-honouring compileFile wrapper
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile4022 {
    std::string path;
    explicit TempFile4022(std::string p) : path(std::move(p)) {}
    ~TempFile4022() { std::remove(path.c_str()); }
};

void writeSource4022(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile lib -> object, compile app (importing lib) -> exe, run it in a
// SEPARATE process, return its exit code:
//   -1 compile/link/run failure (the app did not build)
//   -2 the dependency itself could not be produced (no toolchain)
int compileImportAndRun4022(const std::string& libSrc,
                            const std::string& appSrc) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz4022_" + unique_stem("");
    TempFile4022 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile4022 aObj((dir / (stem + "_a.o")).string());
    writeSource4022(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile4022 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile4022 bExe((dir / stem).string());
    writeSource4022(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    if (!bezel::test::compileFileForTest(driver, bSrc.path, bExe.path,
                                        deps).success) return -1;
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
int runSingleModule4022(const std::string& src) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz4022ctl_" + unique_stem("");
    TempFile4022 sSrc((dir / (stem + ".bzl")).string());
    TempFile4022 sExe((dir / stem).string());
    writeSource4022(sSrc.path, src);
    bezel::Driver driver;
    if (!bezel::test::compileFileForTest(driver, sSrc.path, sExe.path).success)
        return -1;
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

// ───────────────────────────────────────────────────────────────────────────
// The HEADLINE LIBRARY: a record whose 0-arg ctor chains via
// `this(new Widget4022(88))` to a 1-arg ctor that sets a reference-typed
// property. Mirrors the #4013 reference-discharge case, but with the
// `new Widget4022(88)` argument form that defeats the importer-side
// `parseAndAttachCtorBody` reparse (Widget4022 is a SIBLING of R4022A in
// this library — not the record itself, so the importer's single-type
// synthetic source can't resolve `Widget4022` and leaves R4022A's ctor
// `decl` null on the importer side).
// ───────────────────────────────────────────────────────────────────────────
const char* kLib4022A =
    "module Lib4022A; "
    "public class Widget4022A "
    "  public Count: int; "
    "  public constructor(c: int) Count := c; end constructor; "
    "  public method Render(): int return Count; end Render; "
    "end Widget4022A; "
    "public record R4022A "
    "  public readonly property Extra: Widget4022A; "
    "  public constructor(w: Widget4022A) Extra := w; end constructor; "
    "  public constructor() this(new Widget4022A(88)); end constructor; "
    "end R4022A;";

// T1 — IMPORTED headline: c.Extra.Render() must return 88. Pre-fix this
// SIGSEGVs (`exit=139`) on the importer side because the chain never runs
// and `Extra`'s storage stays at the §22.8 null watermark.
const char* kAppT1 =
    "module App4022T1; import Lib4022A; "
    "public class Program4022T1 "
    "  public static method Main(): int "
    "    var c := new R4022A(); "
    "    return c.Extra.Render(); "
    "  end Main; "
    "end Program4022T1;";

// T1b — SINGLE-MODULE twin of T1: identical declarations, identical body,
// NO `import` anywhere. Pre-fix this returns 88 already (#4013's fix made
// it work on the single-module path); post-fix it must continue returning
// 88. The differential is "the only thing that changed is the module
// boundary".
const char* kLocalT1 =
    "module Program4022T1b; "
    "public class Widget4022AT1b "
    "  public Count: int; "
    "  public constructor(c: int) Count := c; end constructor; "
    "  public method Render(): int return Count; end Render; "
    "end Widget4022AT1b; "
    "public record R4022AT1b "
    "  public readonly property Extra: Widget4022AT1b; "
    "  public constructor(w: Widget4022AT1b) Extra := w; end constructor; "
    "  public constructor() this(new Widget4022AT1b(88)); end constructor; "
    "end R4022AT1b; "
    "public class Program4022AT1b "
    "  public static method Main(): int "
    "    var c := new R4022AT1b(); "
    "    return c.Extra.Render(); "
    "  end Main; "
    "end Program4022AT1b;";

// ───────────────────────────────────────────────────────────────────────────
// The CLASS LIBRARY: a class whose 0-arg ctor chains via `this(99)` to a
// 1-arg ctor. Mirrors the #4013 class-side control but the chain is on a
// CLASS — exercising the importer-side `emitChainCall` path inherited
// from `emitConstructorBody`'s `chainCallOf` machinery, which was the
// second defect site for #4022 (the importer's lazy ctor declaration
// landed on the wrong LLVM name slot).
// ───────────────────────────────────────────────────────────────────────────
const char* kLib4022B =
    "module Lib4022B; "
    "public class Widget4022B "
    "  public Count: int; "
    "  public constructor(c: int) Count := c; end constructor; "
    "  public constructor() this(99); end constructor; "
    "  public method Render(): int return Count; end Render; "
    "end Widget4022B;";

// T2 — IMPORTED class case: w.Render() must return 99. Pre-fix this
// returns 16 (the wrong ctor was called — a name-suffix collision made the
// importer's call land on the 1-arg `ctor(ptr, i32)` instead of the
// 0-arg `ctor(ptr)`, and the 4 bytes read back as the return were the
// `99` argument reinterpreted as a field value).
const char* kAppT2 =
    "module App4022T2; import Lib4022B; "
    "public class Program4022T2 "
    "  public static method Main(): int "
    "    var w := new Widget4022B(); "
    "    return w.Render(); "
    "  end Main; "
    "end Program4022T2;";

// T2b — single-module twin: identical declarations, NO `import`. This
// already returns 99 on both sides of the fix — it's the differential
// control that proves T2 is a CROSS-MODULE defect, not a class-chain
// defect.
const char* kLocalT2 =
    "module Program4022T2b; "
    "public class Widget4022BT2b "
    "  public Count: int; "
    "  public constructor(c: int) Count := c; end constructor; "
    "  public constructor() this(99); end constructor; "
    "  public method Render(): int return Count; end Render; "
    "end Widget4022BT2b; "
    "public class Program4022BT2b "
    "  public static method Main(): int "
    "    var w := new Widget4022BT2b(); "
    "    return w.Render(); "
    "  end Main; "
    "end Program4022BT2b;";

// ───────────────────────────────────────────────────────────────────────────
// The INT-CHAIN LIBRARY: a record whose 0-arg ctor chains via `this(42)`
// to a 1-arg ctor. The simpler shape — no reference-typed property, no
// sibling reference — used to pin the §22.8 once-only rule across the
// module boundary.
// ───────────────────────────────────────────────────────────────────────────
const char* kLib4022C =
    "module Lib4022C; "
    "public record R4022C "
    "  public readonly property N: int; "
    "  public constructor(n: int) N := n; end constructor; "
    "  public constructor() this(42); end constructor; "
    "end R4022C;";

// T3 — IMPORTED int chain: c.N must return 42. Pre-fix on the importer
// side `cd->body` reparse happens to SUCCEED here (no sibling-name
// resolution failure), but the AST gate still saw no chain call — the
// `this(42)` reparse still fell into `lowerScopedBlock`, no IR was
// emitted for the chain, and `N` came back 0. (Some importer paths on
// some Bezel revisions accidentally routed this case correctly; #4022 is
// the bug for the OTHER shape — sibling-name reparse failure. T3 is
// included to lock down the simple case so a regression to the simple
// path is caught too.)
const char* kAppT3 =
    "module App4022T3; import Lib4022C; "
    "public class Program4022T3 "
    "  public static method Main(): int "
    "    var c := new R4022C(); "
    "    return c.N; "
    "  end Main; "
    "end Program4022T3;";

const char* kLocalT3 =
    "module Program4022T3b; "
    "public record R4022CT3b "
    "  public readonly property N: int; "
    "  public constructor(n: int) N := n; end constructor; "
    "  public constructor() this(42); end constructor; "
    "end R4022CT3b; "
    "public class Program4022CT3b "
    "  public static method Main(): int "
    "    var c := new R4022CT3b(); "
    "    return c.N; "
    "  end Main; "
    "end Program4022CT3b;";

// ───────────────────────────────────────────────────────────────────────────
// The §22.8 ONCE-ONLY LIBRARY: a record whose ctor has a non-idempotent
// field initializer (a static counter). A double-run of step 2 leaves the
// counter at 2; the spec requires 1.
// ───────────────────────────────────────────────────────────────────────────
const char* kLib4022D =
    "module Lib4022D; "
    "public class Counter4022D "
    "  public static Value: int := 0; "
    "  public static method Next(): int "
    "    Counter4022D.Value := Counter4022D.Value + 1; "
    "    return Counter4022D.Value; "
    "  end Next; "
    "end Counter4022D; "
    "public record R4022D "
    "  public property V: int := Counter4022D.Next(); "
    "  public readonly property N: int; "
    "  public constructor(n: int) N := n; end constructor; "
    "  public constructor() this(5); end constructor; "
    "end R4022D;";

// T3c — IMPORTED §22.8 once-only: Counter must reach exactly 1 (not 2),
// N must be 5. Pre-fix on the importer side the chain target's compiled
// ctor ran Counter.Next() (via its own `!isThis`-gated `emitOwnFieldInit`)
// AND the importer-side preamble ran Counter.Next() a SECOND time — the
// exact §22.8 double-run the spec names ("assigning `readonly` fields
// twice", "repeating allocations and initializer side effects").
const char* kAppT3c =
    "module App4022T3c; import Lib4022D; "
    "public class Program4022T3c "
    "  public static method Main(): int "
    "    var c := new R4022D(); "
    "    return c.V * 100 + c.N; "
    "  end Main; "
    "end Program4022T3c;";

const char* kLocalT3c =
    "module Program4022T3cb; "
    "public class Counter4022DT3cb "
    "  public static Value: int := 0; "
    "  public static method Next(): int "
    "    Counter4022DT3cb.Value := Counter4022DT3cb.Value + 1; "
    "    return Counter4022DT3cb.Value; "
    "  end Next; "
    "end Counter4022DT3cb; "
    "public record R4022DT3cb "
    "  public property V: int := Counter4022DT3cb.Next(); "
    "  public readonly property N: int; "
    "  public constructor(n: int) N := n; end constructor; "
    "  public constructor() this(5); end constructor; "
    "end R4022DT3cb; "
    "public class Program4022DT3cb "
    "  public static method Main(): int "
    "    var c := new R4022DT3cb(); "
    "    return c.V * 100 + c.N; "
    "  end Main; "
    "end Program4022DT3cb;";

// ───────────────────────────────────────────────────────────────────────────
// The NO-CHAIN LIBRARY: a record with NO `this(...)` chain — just a 0-arg
// ctor that assigns a field. Pre- and post-fix this returns 99 on both
// sides; the assertion is its own correctness, and a regression here
// would mean we broke an unrelated path.
// ───────────────────────────────────────────────────────────────────────────
const char* kLib4022E =
    "module Lib4022E; "
    "public record R4022E "
    "  public readonly property N: int := 0; "
    "  public constructor() N := 99; end constructor; "
    "end R4022E;";

const char* kAppT4 =
    "module App4022T4; import Lib4022E; "
    "public class Program4022T4 "
    "  public static method Main(): int "
    "    var c := new R4022E(); "
    "    return c.N; "
    "  end Main; "
    "end Program4022T4;";

}  // namespace

// ===========================================================================
// T1 — HEADLINE: an IMPORTED record whose zero-arg ctor chains via
// `this(new Sibling(88))` to a one-arg ctor that sets a reference-typed
// property. Pre-fix this SIGSEGVs (`exit=139`) on the importer side — the
// chain never runs, `Extra` is null, `.Render()` dereferences it.
// Post-fix it returns 88.
//
// ⚠ This row is the differential — T1b (single-module twin) is its
// control. T1 alone would pass on a compiler that always SIGSEGVed (a
// regression in a different direction) or on one that hard-coded
// `exit=88`; T1b alone would pass on a compiler that already handled
// #4013 but never knew about #4022. Both together pin "the importer
// agrees with the twin".
// ===========================================================================
BEZEL_TEST(xmod_record_this_chain_ctor_imported_4022,
           t1_imported_record_this_chain_discharges_reference_field) {
    int rc = compileImportAndRun4022(kLib4022A, kAppT1);
    if (rc == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(rc, 88);
}

// T1b — single-module twin of T1. Must keep returning 88 on both sides of
// the fix (#4013's own single-module coverage already pins this; the row
// exists here to make the T1/T1b pair a differential claim).
BEZEL_TEST(xmod_record_this_chain_ctor_imported_4022,
           t1b_single_module_record_this_chain_discharges_reference_field) {
    BEZEL_EXPECT_EQ(runSingleModule4022(kLocalT1), 88);
}

// ===========================================================================
// T2 — IMPORTED class: the importer-side `emitChainCall` for an imported
// class ctor declared its compiled function LAZILY (on first call), and
// the lazy declaration landed on a DIFFERENT LLVM name slot than the
// exporter's eager declaration — the call resolved to a function with
// the wrong signature, and the value read back was the wrong ctor's
// return (16, where the spec demands 99).
// ===========================================================================
BEZEL_TEST(xmod_record_this_chain_ctor_imported_4022,
           t2_imported_class_this_chain_returns_correct_value) {
    int rc = compileImportAndRun4022(kLib4022B, kAppT2);
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 99);
}

// T2b — single-module twin of T2. Already returns 99 on both sides of
// the fix (the class-side `inlineCtorBody` was correct for the
// single-module case; #4022 was importer-side only). Pinning it here
// proves T2 is the cross-module defect, not a class-chain defect.
BEZEL_TEST(xmod_record_this_chain_ctor_imported_4022,
           t2b_single_module_class_this_chain_returns_correct_value) {
    BEZEL_EXPECT_EQ(runSingleModule4022(kLocalT2), 99);
}

// ===========================================================================
// T3 — IMPORTED int-chain record: c.N must return 42. Pre-fix on the
// importer side the chain never ran (no sibling reparse failure, but the
// AST gate still saw no chain call); N came back 0.
// ===========================================================================
BEZEL_TEST(xmod_record_this_chain_ctor_imported_4022,
           t3_imported_record_this_chain_int_value) {
    int rc = compileImportAndRun4022(kLib4022C, kAppT3);
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 42);
}

// T3b — single-module twin of T3.
BEZEL_TEST(xmod_record_this_chain_ctor_imported_4022,
           t3b_single_module_record_this_chain_int_value) {
    BEZEL_EXPECT_EQ(runSingleModule4022(kLocalT3), 42);
}

// ===========================================================================
// T3c — IMPORTED §22.8 once-only: a chain composed with a non-idempotent
// field initializer (a static counter). Pre-fix on the importer side
// `Counter.Next()` ran TWICE (once via the chain target's compiled
// ctor's `emitOwnFieldInit`, once via the importer-side preamble);
// post-fix it runs exactly once. The shape is the #4019 single-module
// case extended to the cross-module boundary.
// ===========================================================================
BEZEL_TEST(xmod_record_this_chain_ctor_imported_4022,
           t3c_imported_record_this_chain_field_initializer_runs_once) {
    int rc = compileImportAndRun4022(kLib4022D, kAppT3c);
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 105);  // V=1 (not 2), N=5
}

// T3d — single-module twin of T3c.
BEZEL_TEST(xmod_record_this_chain_ctor_imported_4022,
           t3d_single_module_record_this_chain_field_initializer_runs_once) {
    BEZEL_EXPECT_EQ(runSingleModule4022(kLocalT3c), 105);  // V=1 (not 2), N=5
}

// ===========================================================================
// T4 — IMPORTED no-chain control: an imported record whose 0-arg ctor
// simply assigns a field (no `this(...)`). Must keep returning 99 on both
// sides of the fix — this row exists to catch a regression that breaks
// the unrelated non-chain import path while fixing the chain import path.
// ===========================================================================
BEZEL_TEST(xmod_record_this_chain_ctor_imported_4022,
           t4_imported_record_no_chain_value) {
    int rc = compileImportAndRun4022(kLib4022E, kAppT4);
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 99);
}

}  // namespace bezel::module
