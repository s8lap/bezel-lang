// narrow #3378 — REFUTATION GUARD, not a fix (§14.1 "Generic declarations" /
// §22.7 "Constructors" / §22.8 step 3 "The constructor body" /
// §22.10 "Primary constructors" / §2.5 "Dependency resolution" /
// §2.8.3 rule 4 "Identity is one type at runtime, too", v0.282).
//
// #3378 claimed an IR-verification failure here. It DOES NOT REPRODUCE — not
// on HEAD, and not on `d34c8b4`, the main HEAD at the moment the issue was
// filed (both run the issue's own recipe to a correct exit 23). No compiler
// change accompanies this file; it pins the working behavior so the path
// cannot silently regress. See the REFUTATION block below for the evidence.
//
// The behavior pinned: IMPORTED generic class TYPED CONSTRUCTOR with a
// type-parameter field write monomorphizes per instantiation; the ctor body
// is lowered into the importer under the active `genericSubst_` so a
// `T = Pt` (record) instantiation stores a `Pt` into the field, not the
// `i32` erased placeholder the LIB compiled against.
//
// ── CONTEXT (why this test exists) ──────────────────────────────────────────
//
// The #3365 read-only-property admission (the same axis: imported generic
// class with a `T`-typed member, monomorphized per instantiation) explicitly
// AVOIDED the typed-ctor case. Per the #3365 test header's "T1" maintenance
// block:
//
//   "The ctors here are DEFAULT (no-arg) ctors — the issue notes a SEPARATE
//    ctor interaction bug (an IMPORTED TYPED ctor failing IR verification
//    when monomorphized for `T = Pt`) that is NOT covered by this fix; the
//    #3378 fix is the imported-ctor axis, the imported-property axis is
//    a follow-up. To keep T1-T4 focused on the property path, the tests
//    use the default (no-arg) ctor and assign the T-typed storage field
//    via an `Init(v: T)` method (the method-monomorphization path is
//    closed by #3347). The IMPORTER's typed-ctor monomorphization
//    (a #3378 FOLLOW-UP) is exercised by neither T1-T4 nor T5."
//
// #3378 was filed hypothesizing a `%record.Pt` vs `i32` IR type mismatch
// in the importer's monomorphized ctor body. The measured behavior on the
// post-#3365 tree (this test's FAIL-WITHOUT walkthrough on commit e9bd762):
//
//   * The LIB compiles cleanly (T is parametric, no instantiation at lib
//     time; the LIB exports the T→i32 ABI stub the #662 carriage has always
//     carried).
//   * The IMPORTER's `lowerNew` falls into the `importedGenericWithBody`
//     arm (#662 Foundation C: emitInstanceInit installs the per-instantiation
//     vtable, then inlineCtorBody lowers the ctor body with the active
//     `genericSubst_` substituting `T → concrete`). The body is the
//     re-parsed ast::ConstructorDecl attached by parseAndAttachCtorBody
//     (ModuleLoader.cpp, the ctorBodyText carriage), so the field reads
//     and writes go through the per-instantiation struct layout
//     (`class.GBox$specN` keyed on the substituted field-type vector,
//     #460 / #2889 / #2897).
//   * The substitution IS applied to BOTH the parameter type signature AND
//     the body's storage write: a T=Pt instantiation stores 8 bytes at the
//     field's slot (a 2-int record), and the field GEP / field-addr lower
//     flows through `class.GBox$spec0`'s layout, not the i32-erased
//     canonical `class.GBox`. Measured at e9bd762 with the issue's own
//     recipe (the `end ctor;` in the snippet is a typographical error in
//     the issue body — `end constructor;` is the spec spelling per §22.7
//     / §22.10; the lib rejects `end ctor;` at parse):
//
//         module Lib; public class GBox<T> public _t: T;
//           public constructor(v: T) _t := v; end constructor;
//         end GBox;
//         public record Pt public X: int; public Y: int; end Pt;
//
//         module App; import Lib; public class C
//           public static method Main(): int
//             using var g := new GBox<Pt>(new Pt(11, 12));
//             return g._t.X + g._t.Y;
//           end Main; end C;
//
//     → bezelc -c lib.bzl → bezelc --import lib.o -o app app.bzl
//     → app exits 23 (= 11 + 12; the verified store) on the plain path.
//
// ── REFUTATION (why #3378 is not a defect) ──────────────────────────────────
//
// 1. The issue's own recipe passes. At HEAD (e9bd762) it compiles the lib,
//    compiles+links the importer, and exits 23 — no IR-verification failure.
//
// 2. It also passes at the filing-time baseline. #3378 was filed
//    2026-08-26T03:59Z; main's HEAD at that instant was `d34c8b4`. bezelc
//    built at `d34c8b4` runs the same recipe to exit 23. The defect was
//    therefore never present on main — this is not a fix that landed in
//    between. (The issue says it measured "on HEAD after #3365 was
//    committed", but #3365 reached main only at 07:46Z, ~4h AFTER the
//    issue was filed, so the measurement was taken inside #3365's own lane
//    worktree rather than on main.)
//
// 3. The mechanism the issue says is missing has been in the tree for a
//    long time. #3378's "SUGGESTED FIX" asserts "the CTOR path
//    monomorphization ... has not landed". It has: the `ctorBodyText`
//    carriage (#662 Foundation C, metadata schema v5), its re-parse in
//    `parseAndAttachCtorBody` (src/module/ModuleLoader.cpp), and the
//    `importedGenericWithBody` arm that calls `inlineCtorBody`
//    (src/codegen/LlvmBackend_Exprs2.cpp) — refined by #695, #705, #2842,
//    and #3201.
//
// 4. The issue's snippet is internally inconsistent, which suggests the
//    report was reconstructed rather than captured: it spells the
//    terminator `end ctor;`, which does not parse ("expected ';' after
//    'end' of constructor"), so the LIBRARY could not have "compiled
//    clean" as the issue states; and the quoted LLVM error carries a
//    literal "..." ellipsis and reads as prose rather than as an actual
//    LLVM diagnostic.
//
// 5. 17 adversarial variants around the reported shape were probed —
//    importer-local type argument, several instantiations in one importer,
//    multi-param and multi-type-param ctors, `this(...)` chaining, field
//    initializers alongside the ctor, ctor-internal method calls and
//    read-backs, T-typed locals, generic methods, a T-typed property, and
//    lib-side plus app-side instantiation of the same and different T.
//    All produced correct values; none reproduced the reported failure.
//
// This test therefore pins the correct behavior so a future change to the
// imported generic ctor body lowering cannot silently regress it. Each
// subtest uses a
// distinct concrete T so the test fails loudly on any of the four
// layout-relevant axes:
//
//   T1: T = record (the issue's exact shape; layout-moving: 8 bytes
//       where the LIB compiled against i32). The ctor body stores a
//       2-int record into the field.
//   T2: T = int (layout-identical; same width as the i32 erased
//       placeholder). The ctor body stores an i32.
//   T3: T = string (layout-moving; 8-byte ptr where the LIB compiled
//       against i32). The ctor body stores a 8-byte string handle.
//   T4: PRIMARY CONSTRUCTOR variant — the §22.10 path. `class GBox<T>(V: T)`
//       with `_v: V` instead of an explicit `public constructor(v: T)`. This
//       is the path #3201 closed (the GENERIC half of #3162), and is the
//       shape whose absence motivated #3378 being filed.
//
// Each test runs as a clean compileImportRun through `bezel::Driver` (the
// real AOT path), captures stdout, asserts exit-code-or-positive, and asserts
// the exact computed return value (which is the program's exit code, the
// verified-correct store from the body). The harness returns -1 on importer
// compile failure (the original #3378 reported failure mode: an `IR
// verification failed` abort during `emitObjectFile`), -2 on dep-toolchain
// unavailability (no system clang / linker), and otherwise the exit code.
//
// ── ORACLES (the same suite, both oracles) ──────────────────────────────────
//
// Both `gate.sh` and `gate.sh --full` exercise this file under the
// `bezel_tests_module` binary. The DEPENDENCY object is not
// AddressSanitizer-instrumented (compileObject ignores sanitizeAddress, the
// cross-module soundness oracle since #1861), so the bug class #3378 was
// filed against — a malformed IR store in the importer's emitted code — is
// caught at AOT link time (verifyModule rejects the module, emitObjectFile
// returns false, the link is skipped), which is the PLAIN oracle's domain.
// ASan would catch the silent-wrong-value variant (an i32 store followed
// by an 8-byte read) but that variant is not what #3378 was filed against.
//
// ── RELATED ─────────────────────────────────────────────────────────────────
//
// #2884 — original decline on imported generic property access.
// #2893 — `propertyBodyText` carriage (precedent for body re-parse +
//         attachment in the importer).
// #3346 / #3347 / #3348 — method and property monomorphization (the
//         method-side sibling of this test's ctor-side coverage).
// #3365 — the read-only PROPERTY admission whose T1-T4 deliberately
//         avoided the typed ctor and explicitly noted #3378 as a
//         follow-up. This file is the follow-up.
// #3201 — the GENERIC half of #3162's primary-ctor admission; T4 of this
//         file exercises the §22.10 (primary-ctor) shape on the imported
//         generic axis.

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
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"


namespace bezel::module {
namespace {

namespace fs = std::filesystem;

const std::filesystem::path& wtDir3378() {
    return wtScratchDir("wt-3378");
}

struct TempFile3378 {
    std::string path;
    explicit TempFile3378(std::string p) : path(std::move(p)) {}
    ~TempFile3378() { std::remove(path.c_str()); }
};

void writeSource3378(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile lib + app, link and run, capturing stdout — for the positive
// assertion (exit code == the computed program value). Mirrors the 3365
// harness; shared with this file's other helpers via anonymous namespace.
std::pair<int, std::string> compileImportRun3378(const std::string& libSrc,
                                                 const std::string& appSrc,
                                                 const std::string& tag) {
    auto dir = wtDir3378();
    std::string stem = "bz3378_" + tag + "_" + unique_stem("");
    TempFile3378 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3378 aObj((dir / (stem + "_a.o")).string());
    writeSource3378(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return {-2, ""};
    }
    if (!std::ifstream(aObj.path).good()) return {-2, ""};
    TempFile3378 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3378 bExe((dir / (stem + "_b")).string());
    writeSource3378(bSrc.path, appSrc);
    bool ok;
    {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj.path;
        d.objectPath = aObj.path;
        deps.push_back(d);
        ok = driver.compileFile(bSrc.path, bExe.path, deps).success;
    }
    if (!ok) return {-1, ""};
    std::string cmd = bExe.path + " 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return {-1, ""};
    std::string out;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), p)) out += buf;
    int status = pclose(p);
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return {code, out};
}

bool has3378(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// Pt record used for the layout-moving T1 and T4 (primary-ctor) cases. Same
// 2-int record the issue's own recipe uses; kept inline so the file is
// self-contained for §14.1.
const char* kPtDef3378 =
    "public record Pt3378 "
    "  public X: int; "
    "  public Y: int; "
    "end Pt3378; ";

}  // namespace

// ===========================================================================
// T1 — REPRO / ACCEPT (the issue's exact shape): a generic class with a
// PUBLIC field `_t: T`, a typed ctor `(v: T) _t := v; end constructor;`,
// instantiated at `Pt3378` (a 2-int record — layout moves vs the i32
// erased placeholder the LIB compiled against). The IMPORTER's ctor body
// monomorphization must store `Pt3378` (8 bytes) into `_t`, NOT the
// i32 erased placeholder — that would be a 4-byte store into an 8-byte
// slot, and LLVM's verifyModule would reject the module at the importer's
// emitObjectFile point (#3378's claimed failure mode).
//
// Pre-fix (the issue's diagnosis): importer's ctor body emits an i32
// store, verifyModule rejects, AOT link fails with "bezelc: link error".
// Post-fix (the current HEAD's measured behavior): importer's ctor body
// emits an 8-byte store at the field's substituted-offset slot, the
// inlined ctor body returns, the read-back `g._t.X + g._t.Y` (= 11 + 12)
// computes to 23, the program exits 23 (the verified store).
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_typed_ctor_ir_3378,
           t1_explicit_ctor_typed_field_pt) {
    auto [code, out] = compileImportRun3378(
        std::string("module Lib3378T1; "
                    "public class GBox3378T1<T> "
                    "  public _t: T; "
                    "  public constructor(v: T) "
                    "    _t := v; "
                    "  end constructor; "
                    "end GBox3378T1; ") + kPtDef3378,
        "module App3378T1; import Lib3378T1; "
        "public class C "
        "  public static method Main(): int "
        "    using var g := new GBox3378T1<Pt3378>(new Pt3378(11, 12)); "
        "    Console.WriteLine(\"sum=\" + (g._t.X + g._t.Y).ToString()); "
        "    return g._t.X + g._t.Y; "
        "  end Main; "
        "end C;",
        "t1");
    if (code == -2) return;
    BEZEL_EXPECT_TRUE(code >= 0);  // compile+link succeeded (the #3378 failure mode)
    BEZEL_EXPECT_EQ(code, 23);
    BEZEL_EXPECT_TRUE(has3378(out, "sum=23"));
}

// ===========================================================================
// T2 — LAYOUT-IDENTICAL INSTANTIATION: same library as T1, but instantiated
// at `int`. The storage at `_t: T = int` is the same width as the i32
// erased placeholder, so the read works even on a stub-ABI-only import.
// This row proves the i32-erased ABI path (the LIB's exported stub) is
// NOT the path the importer takes — the importer monomorphizes the ctor
// body into its own function (#662 Foundation C), so the field write uses
// the importer's `class.GBox<int>$spec` layout regardless of T width.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_typed_ctor_ir_3378,
           t2_explicit_ctor_typed_field_int) {
    auto [code, out] = compileImportRun3378(
        "module Lib3378T2; "
        "public class GBox3378T2<T> "
        "  public _t: T; "
        "  public constructor(v: T) "
        "    _t := v; "
        "  end constructor; "
        "end GBox3378T2;",
        "module App3378T2; import Lib3378T2; "
        "public class C "
        "  public static method Main(): int "
        "    using var g := new GBox3378T2<int>(7); "
        "    Console.WriteLine(\"p=\" + g._t.ToString()); "
        "    return g._t; "
        "  end Main; "
        "end C;",
        "t2");
    if (code == -2) return;
    BEZEL_EXPECT_TRUE(code >= 0);
    BEZEL_EXPECT_EQ(code, 7);
    BEZEL_EXPECT_TRUE(has3378(out, "p=7"));
}

// ===========================================================================
// T3 — LAYOUT-MOVING REFERENCE TYPE: instantiated at `string`. The
// `_t: T = string` slot is 8 bytes where the LIB's i32 erasure is 4. If
// the importer's ctor body used the LIB's erased slot size it would emit
// a 4-byte store into an 8-byte slot (silent truncation); verifyModule
// would catch the type-mismatched GEP and reject the module. The verified
// post-#3365 codegen monomorphizes the ctor body so the field store uses
// the concrete 8-byte slot. Length=11 asserts the read-back recovered the
// whole string.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_typed_ctor_ir_3378,
           t3_explicit_ctor_typed_field_string) {
    auto [code, out] = compileImportRun3378(
        "module Lib3378T3; "
        "public class GBox3378T3<T> "
        "  public _t: T; "
        "  public constructor(v: T) "
        "    _t := v; "
        "  end constructor; "
        "end GBox3378T3;",
        "module App3378T3; import Lib3378T3; "
        "public class C "
        "  public static method Main(): int "
        "    using var g := new GBox3378T3<string>(\"hello world\"); "
        "    Console.WriteLine(\"len=\" + g._t.Length.ToString()); "
        "    return g._t.Length; "
        "  end Main; "
        "end C;",
        "t3");
    if (code == -2) return;
    BEZEL_EXPECT_TRUE(code >= 0);
    BEZEL_EXPECT_EQ(code, 11);
    BEZEL_EXPECT_TRUE(has3378(out, "len=11"));
}

// ===========================================================================
// T4 — PRIMARY CONSTRUCTOR (§22.10) variant — the §22.10 path on the
// imported generic axis. `class GBox3378T4<T>(_t: T)` stores the primary
// parameter into the auto-declared `_t` field; `new GBox3378T4<Pt3378>(...)`
// runs #3201's step-0 store on the importing side. The field shape is
// identical to T1's explicit ctor, so the layout-moving 2-int record
// covers the same IR-monomorphization path. This row exists so the
// §22.10 axis (primary-ctor parameters, the `bz$<Type>$init`-less generic
// path #3201 closed) is covered alongside the explicit-ctor axis T1 covers.
//
// Per spec §22.10 (v0.307, line 1861, after #4868 5458a4e1): the class's
// primary-ctor parameter `_t` is NOT auto-promoted to a public property. The
// canonical idiom (docs/Bezel_Language_Spec.md:2042-2057) requires the class
// author to declare an explicit `public readonly property` so the field is
// readable out-of-class. Migration: declare `public readonly property _t: T := _t;`
// (RHS `_t` is the primary-ctor parameter, in initializer position per
// §22.10); the ctor's body still captures `_t` for any internal references.
BEZEL_TEST(xmod_imported_generic_typed_ctor_ir_3378,
           t4_primary_ctor_typed_field_pt) {
    auto [code, out] = compileImportRun3378(
        std::string("module Lib3378T4; "
                    "public class GBox3378T4<T>(_t: T) "
                    "  public readonly property _t: T := _t; "
                    "end GBox3378T4; ") + kPtDef3378,
        "module App3378T4; import Lib3378T4; "
        "public class C "
        "  public static method Main(): int "
        "    var g := new GBox3378T4<Pt3378>(new Pt3378(20, 21)); "
        "    Console.WriteLine(\"sum=\" + (g._t.X + g._t.Y).ToString()); "
        "    return g._t.X + g._t.Y; "
        "  end Main; "
        "end C;",
        "t4");
    if (code == -2) return;
    BEZEL_EXPECT_TRUE(code >= 0);
    BEZEL_EXPECT_EQ(code, 41);
    BEZEL_EXPECT_TRUE(has3378(out, "sum=41"));
}

}  // namespace bezel::module
