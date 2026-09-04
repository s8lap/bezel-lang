// Codegen — issue #3645 / spec §9.7 ("Records and disposal") + §28.9
// ("Reference counting of runtime-managed values") + §2.8.3 rule 4
// ("Cross-module derivation"), docs revision v0.286.
//
// REGRESSION PIN. The structural fix removes #3621's signature-only sibling
// (`isProvablyFreshImportedRecordFactoryMethod_`, LlvmBackend_Setup.cpp),
// which admitted any IMPORTED non-generic STATIC method whose wire-side
// return type exactly matched `recTs` and whose record had no explicit
// ctor — a soundness hole that #2448's body-walk proof closed for locals
// but the signature-only sibling silently re-opened for imports.
//
// The body-walk proof `isProvablyFreshRecordFactoryMethod` (the LOCAL
// predicate, equally applicable to imports once the body ships on the
// wire) covers MORE imported shapes than the signature sibling did,
// because it verifies the body's `new P(...)` construction directly:
//   - INSTANCE methods whose body is `=> new P(...)` (signature sibling
//     rejects on guard (a) — must be static — so a fresh-construction
//     INSTANCE factory was UNADMITTED on the parent, body-shipping or
//     not, leaving a per-iteration string-field leak);
//   - GENERIC methods (already shipped via §14.1 / #783 pre-#3645, this
//     row pins that the new path does not regress the #783 canary).
//
// The fix ships `methodBodyText` for a non-generic class method whose
// return type resolves to a record class so the importer can re-parse +
// attach it. With the body attached, the body's NewExpr of exactly
// `recTs` is provable from anywhere, so the body walk is the single
// source of truth for BOTH locals AND imports — and the parent’s
// signature-only sibling is gone.
//
// LOCALS (`ms->decl != nullptr` from the outset) are unchanged: the
// body walk already fires.
//
// ── FIX SHAPE (spec §9.7 / §28.9 / §2.8.3 rule 4) ───────────────────────────
//
// Three coordinated changes:
//   (a) EXPORTER (`src/module/ModuleMetadataBuilder.cpp` MethodDecl arm):
//       the gate on shipping `methodBodyText` widens from
//       `isGenericType && ...` to
//       `(isGenericType || (methodReturnsRecord && md->genericParams.empty())) && ...`
//       The `md->genericParams.empty()` narrow is intentional — shipping a
//       body for a generic method would set `ms->decl` non-null and silently
//       bypass the §14.1 / #1937 sema gate; the canary for the generic arm
//       is the box in row 4.
//   (b) IMPORTER (`src/module/ModuleLoader.cpp`):
//       (i)  `loadMetadataPass2`'s class gate widens to also admit a
//            NON-GENERIC class whose members carry a non-empty
//            `methodBodyText` entry (the §14.1/#783 generic-only gate
//            would otherwise skip them). Records are unchanged (they
//            already processed non-generic instances).
//       (ii) `buildSyntheticMethodSource` takes a `nonGenericRecordFactoryPath`
//            and emits `import <moduleName>;` at the head of the synthetic
//            source — the cross-module sibling-type visibility trick the
//            field-init path already uses (#2375). Without this, the
//            body's reference to a sibling record (the common case) fails
//            to resolve and the §14.1 / #783 SOUNDNESS GATE refuses to
//            attach the decl, defeating the whole structural fix.
//       (iii) `parseAndAttachMethodBody` registers the exporting module's
//             PUBLIC sibling types into the fresh context
//             (`freshCtx.registerImportedType(...)`). The
//             `nonGenericRecordFactoryPath` flag keeps the existing
//             §14.1/#783 generic-method behavior unchanged (no import,
//             no sibling registry) so the existing T5 canary still
//             rejects.
//   (c) CODEGEN (`src/codegen/LlvmBackend_Setup.cpp`): the signature-only
//       sibling `isProvablyFreshImportedRecordFactoryMethod_` is removed
//       AND a `// SEAM-ACCESSOR: …` marker is re-added above the
//       `ms->decl` read inside `isProvablyFreshRecordFactoryMethod` to
//       keep the imported-symbol ratchet honest (one read is the
//       sanctioned source of truth for locals AND imports — not an
//       AST-gated gap). The body walk is the single source of truth
//       for both locals and imports.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::codegen {
namespace {

namespace fs = std::filesystem;

// Per-process unique working dir under the shared scratch base (DISK-backed
// per CLAUDE.md). The library object MUST outlive the app build — a removed
// .o shows up as `bezelc: error: cannot load module metadata from ...`. The
// `TempFile` shared_ptrs in the helpers below keep it open.
static const std::filesystem::path& wtDir3645() {
    return wtScratchDir("wt-3645");
}

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& p) : path(p) {}
    ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream(path) << src;
}

struct LibHandle {
    std::shared_ptr<TempFile> src;
    std::shared_ptr<TempFile> obj;
    std::string objPath;
    bool ok = false;
};

LibHandle buildLib(const fs::path& dir, const std::string& libSrc) {
    LibHandle h;
    std::string stem = unique_stem("bezel_3645lib_");
    h.src = std::make_shared<TempFile>((dir / (stem + "_lib.bzl")).string());
    h.obj = std::make_shared<TempFile>((dir / (stem + "_lib.o")).string());
    writeSource(h.src->path, libSrc);
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileObject(h.src->path, h.obj->path);
    h.ok = r.success;
    h.objPath = h.obj->path;
    return h;
}

struct AppHandle {
    std::shared_ptr<TempFile> src;
    std::shared_ptr<TempFile> exe;
    std::string exePath;
    bool ok = false;
};

AppHandle buildApp(const fs::path& dir, const std::string& libObjPath,
                   const std::string& appSrc, bool sanitize) {
    AppHandle h;
    if (libObjPath.empty()) return h;
    std::string stem = unique_stem("bezel_3645app_");
    h.src = std::make_shared<TempFile>((dir / (stem + "_app.bzl")).string());
    h.exe = std::make_shared<TempFile>((dir / stem).string());
    writeSource(h.src->path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObjPath;
    d.objectPath = libObjPath;
    deps.push_back(d);
    bezel::codegen::CodegenOptions opts;
    opts.sanitizeAddress = sanitize;
    bezel::CompileResult rb = driver.compileFile(h.src->path, h.exe->path,
                                                  deps, opts);
    h.ok = rb.success;
    h.exePath = h.exe->path;
    return h;
}

struct LocalHandle {
    std::shared_ptr<TempFile> src;
    std::shared_ptr<TempFile> exe;
    std::string exePath;
    bool ok = false;
};

LocalHandle buildLocalApp(const fs::path& dir, const std::string& src,
                          bool sanitize) {
    LocalHandle h;
    std::string stem = unique_stem("bezel_3645local_");
    h.src = std::make_shared<TempFile>((dir / (stem + ".bzl")).string());
    h.exe = std::make_shared<TempFile>((dir / stem).string());
    writeSource(h.src->path, src);
    bezel::Driver driver;
    bezel::codegen::CodegenOptions opts;
    opts.sanitizeAddress = sanitize;
    bezel::CompileResult r =
        driver.compileFile(h.src->path, h.exe->path, {}, opts);
    h.ok = r.success;
    h.exePath = h.exe->path;
    return h;
}

struct RunResult { int code; std::string out; };

RunResult runCapture(const std::string& exe) {
    std::string out;
    FILE* p = popen((exe + " 2>&1").c_str(), "r");
    if (!p) return {-1, out};
    char buf[256];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p)) out.append(buf, n);
    int status = pclose(p);
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return {code, out};
}

// Row 1 — HEADLINE: IMPORTED INSTANCE-method fresh-construction factory.
//
// A `Maker.Make(n: int): Pt => new Pt(n, "hello-" + n.ToString())` is
// called via a holder instance. The body walk's
// `isFreshRecordConstructionOfExactType` verifies the body is exactly a
// fresh `Pt` construction, requires every string-typed positional arg
// to be either a literal OR an `isPlusOneStringProducer` (so a literal
// would not exercise the release path) — and does NOT require the
// method to be `static`. The parent’s signature-only sibling DID
// require `static` (guard (a)), so the imported INSTANCE twin went
// UNADMITTED on the parent: `recordCallResultReleaseEligible_` never
// received the local, the scope-end `recordStringFields` Disposal never
// registered, and the per-iteration +1 on `p.Name` outlived the inner
// scope.
//
// Measured pre-#3645 leak against `3f044f3`: each iteration produces a
// fresh `Pt` whose `Name` is a +1 owned by the call result. With the
// release disabled, the inner `do … end` drops the local without
// releasing the +1. The exact byte count is allocator-dependent; the
// load-bearing invariant is that AT LEAST ONE `Direct leak of N byte(s)
// in M object(s)` attributed to `bz$Lib3645` (the importer-side
// records) appears, with M scaling with the iteration count.
//
// Post-#3645: 0 leaks.
//
// Note: the instance method requires a `Maker` holder to dispatch on.
// The holder itself owns a `Pt` field initialized via the local primary
// ctor (no nested string field shape beyond the one being tested), and
// we never invoke the holder's discard-empty constructor or its seed
// shape past construction. The seed uses a literal `Name` so the
// holder’s own +1 bookkeeping is irrelevant; we ONLY count the per-call
// +1 minted by `Make`.
const std::string kInstLibSrc3645 =
    "module Lib3645; "
    "public record Pt(X: int, Name: string); "
    "public class Maker3645 "
    "  public constructor(p: Pt) end constructor; "
    "  public method Make(n: int): Pt => new Pt(n, \"hello-\" + n.ToString()); "
    "end Maker3645; ";

const std::string kInstAppSrc3645 =
    "module App3645; "
    "import Lib3645; "
    "public class C3645 "
    "  public static method Main(): int "
    "    var m := new Lib3645.Maker3645(new Pt(7, \"seed\")); "
    "    var total: int := 0; "
    "    var i: int := 0; "
    "    while i < 200 do "
    "      do "
    "        var p := m.Make(i); "
    "        total := total + p.Name.Length; "
    "      end; "
    "      i := i + 1; "
    "    end while; "
    "    return total; "
    "  end Main; "
    "end C3645; ";

// Row 2 — control: LOCAL twin (character-identical single-module shape).
// The body walk admits this through the local `ast::MethodDecl` path
// (unchanged by #3645). Pins that the structural fix does not disturb
// the local path.
const std::string kLocalSrc3645 =
    "module Local3645; "
    "public record Pt(X: int, Name: string); "
    "public class Maker3645 "
    "  public constructor(p: Pt) end constructor; "
    "  public method Make(n: int): Pt => new Pt(n, \"hello-\" + n.ToString()); "
    "end Maker3645; "
    "public class C3645 "
    "  public static method Main(): int "
    "    var m := new Maker3645(new Pt(7, \"seed\")); "
    "    var total: int := 0; "
    "    var i: int := 0; "
    "    while i < 200 do "
    "      do "
    "        var p := m.Make(i); "
    "        total := total + p.Name.Length; "
    "      end; "
    "      i := i + 1; "
    "    end while; "
    "    return total; "
    "  end Main; "
    "end C3645; ";

// Row 3 — SOUNDNESS: an imported borrow-returning method. The method's
// body is `return this.f;` — a read of an instance field of type `Pt`,
// NOT a fresh `new Pt(...)` construction. The body walk's
// `isFreshRecordConstructionOfExactType` MUST reject this shape (the
// body is a MemberAccessExpr `this.f`, not a NewExpr). No release is
// registered; the record's storage stays owned by the holder's `f`
// field for the lifetime of the holder — accessed safely after the
// inner-scope local `p` goes out of scope.
//
// Pre-#3645 (with `isProvablyFreshImportedRecordFactoryMethod_` STILL in
// place): the signature sibling rejects an INSTANCE method (guard (a)
// requires `static`), so the row was already unadmitted on the parent —
// no UAF, no leak. Post-#3645: the body walk continues to reject the
// non-NewExpr body; no release; no UAF, no leak. The row therefore
// DOES pin the soundness delta (a release registered on a borrow-return
// would double-free the `f` field at holder destruction time), but does
// not depend on the leak/uaf axis alone.
//
// Note: `Make` is not declared `static` — guard (a) of #3621's sibling
// blocks it. A static borrow-returning factory (`static F(s: Pt): Pt =>
// s`) is structurally impossible (an IdentifierExpr body is rejected by
// the body walk for the same reason as `this.f`), and would be a §14.1
// borrow-rule violation anyway.
const std::string kBorrowLibSrc3645 =
    "module BorrowLib3645; "
    "public record Pt(X: int, Name: string); "
    "public class Holder3645 "
    "  public F: Pt; "
    "  public constructor(f: Pt) this.F := f; end constructor; "
    "  public method Get(): Pt return this.F; end Get; "
    "end Holder3645; ";

const std::string kBorrowAppSrc3645 =
    "module BorrowApp3645; "
    "import BorrowLib3645; "
    "public class C3645 "
    "  public static method Main(): int "
    "    var h := new BorrowLib3645.Holder3645(new Pt(7, \"borrow-row3-seed\")); "
    "    var total: int := 0; "
    "    var i: int := 0; "
    "    while i < 200 do "
    "      do "
    "        var p := h.Get(); "
    "        total := total + p.X; "
    "      end; "
    "      i := i + 1; "
    "    end while; "
    "    if h.F.X <> 7 then return 11; end; "
    "    if h.F.Name <> \"borrow-row3-seed\" then return 12; end; "
    "    return total; "
    "  end Main; "
    "end C3645; ";

// Row 4 — control: IMPORTED GENERIC factory class. The §14.1 / #783
// generic-only arm is the path that the loader-gate widening could
// regress — if `loadMetadataPass2` admitted a generic class without
// going through the generic-method attach path, the #783 canary (a
// `Box<T>.GetDirty` referencing a sibling `Helper.Compute()`) would
// re-enable the body walk prematurely. A non-generic class with NO
// record-returning method has no `methodBodyText` to ship, so it does
// NOT enter the new path at all — the importer leaves it byte-for-byte
// as the parent. Pins that the structural fix does NOT widen the §14.1
// generic path and does NOT newly ship bodies for non-record-returning
// methods.
const std::string kGenericLibSrc3645 =
    "module GenericLib3645; "
    "public record Pt(X: int, Name: string); "
    "public class Box3645 "
    "  public static method Description(): string => \"no-record-return\"; "
    "end Box3645; ";

const std::string kGenericAppSrc3645 =
    "module GenericApp3645; "
    "import GenericLib3645; "
    "public class C3645 "
    "  public static method Main(): int "
    "    var s := GenericLib3645.Box3645.Description(); "
    "    return s.Length; "
    "  end Main; "
    "end C3645; ";

}  // namespace

// ── Row 1 — HEADLINE: imported instance-method factory, ASan no leak ─────
//
// Pre-fix (`3f044f3`, with `isProvablyFreshImportedRecordFactoryMethod_`
// and the body NOT shipped): instance method rejected by guard (a),
// body walk also rejects (ms->decl null) — neither path admits the call,
// no scope-end release, per-iteration string-field leak attributed to
// `bz$Lib3645` records. 200 iterations across an inner `do … end`
// scope produces a `Direct leak of N byte(s) in M object(s)` whose `M`
// scales with the iteration count.
//
// Post-fix: body shipped + body walk admits the fresh-construction shape
// irrespective of static-ness, scope-end release registers, no leak.
BEZEL_TEST(imported_record_factory_method_body_shipping_3645,
           imported_instance_method_factory_asan_no_leak) {
    auto dir = wtDir3645();
    LibHandle libH = buildLib(dir, kInstLibSrc3645);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    AppHandle appH = buildApp(dir, libH.objPath, kInstAppSrc3645,
                              /*sanitize=*/true);
    BEZEL_EXPECT_TRUE(appH.ok);
    if (!appH.ok) return;
    RunResult rr = runCapture(appH.exePath);
    // Exit-code oracle: the program returns `total`, the sum of
    // `Name.Length` over 200 iterations = variable (depends on the
    // length of "hello-" + n.ToString()). The pre-fix path returns this
    // same value; the load-bearing oracle is the LSan soundness line.
    BEZEL_EXPECT_TRUE(rr.code >= 0);
    BEZEL_EXPECT_TRUE(rr.out.find("LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(rr.out.find("Direct leak") == std::string::npos);
    BEZEL_EXPECT_TRUE(rr.out.find("Indirect leak") == std::string::npos);
    BEZEL_EXPECT_TRUE(rr.out.find("bz$Lib") == std::string::npos);
}

// ── Row 2 — control: LOCAL twin, ASan no leak (no-op pin) ─────────────────
//
// Pins that the local path is unchanged by the structural fix. The local
// body walk (`isProvablyFreshRecordFactoryMethod` on the locally-populated
// `ast::MethodDecl`) admits the same body shape — instance-method fresh
// construction; the scope-end release is unchanged; no leak.
BEZEL_TEST(imported_record_factory_method_body_shipping_3645,
           local_twin_still_clean_under_asan) {
    auto dir = wtDir3645();
    LocalHandle h = buildLocalApp(dir, kLocalSrc3645, /*sanitize=*/true);
    BEZEL_EXPECT_TRUE(h.ok);
    if (!h.ok) return;
    RunResult rr = runCapture(h.exePath);
    BEZEL_EXPECT_TRUE(rr.code >= 0);
    BEZEL_EXPECT_TRUE(rr.out.find("LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(rr.out.find("Direct leak") == std::string::npos);
    BEZEL_EXPECT_TRUE(rr.out.find("Indirect leak") == std::string::npos);
}

// ── Row 3 — SOUNDNESS: imported borrow-returning method, ASan no UAF ──────
//
// An instance method whose body is `return this.F;` — a borrow of the
// holder's `F`, not a fresh construction. The body walk correctly
// REJECTS this shape (the body is a MemberAccessExpr, not a NewExpr).
// No release is registered; the borrow ends naturally with the field's
// own scope (holder construction). No UAF (no double-free of `F`'s
// string fields), no leak.
BEZEL_TEST(imported_record_factory_method_body_shipping_3645,
           imported_borrow_returning_method_asan_safe) {
    auto dir = wtDir3645();
    LibHandle libH = buildLib(dir, kBorrowLibSrc3645);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    AppHandle appH = buildApp(dir, libH.objPath, kBorrowAppSrc3645,
                              /*sanitize=*/true);
    BEZEL_EXPECT_TRUE(appH.ok);
    if (!appH.ok) return;
    RunResult rr = runCapture(appH.exePath);
    BEZEL_EXPECT_TRUE(rr.code >= 0);
    // No UAF signature (ASan would print "AddressSanitizer: heap-use-after-free"
    // or "double-free" with a stack trace; LSan would NOT trigger here because
    // the borrowed record's storage is owned by `h.F` which releases once).
    BEZEL_EXPECT_TRUE(rr.out.find("AddressSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(rr.out.find("LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(rr.out.find("Direct leak") == std::string::npos);
    BEZEL_EXPECT_TRUE(rr.out.find("Indirect leak") == std::string::npos);
    BEZEL_EXPECT_TRUE(rr.out.find("heap-use-after-free") == std::string::npos);
    BEZEL_EXPECT_TRUE(rr.out.find("double-free") == std::string::npos);
}

// ── Row 4 — control: non-record-returning class, ASan no leak (no-op pin)
// ─
//
// A non-generic class with NO record-returning method (its single method
// returns `string`, not a record) does NOT enter the new path — there is
// no `methodBodyText` to ship. The exporter's widened gate
// `(isGenericType || (methodReturnsRecord && md->genericParams.empty()))`
// does NOT fire for `Description()` (its return type is `string`, not a
// record class). The importer's `loadMetadataPass2` widening admits a
// class whose `td.members` carry a non-empty `methodBodyText` — empty
// here, the class is rejected back into the §14.1/#783 no-method-body
// arm. Pins that the structural fix does NOT newly wire bodies for
// non-record-returning methods.
BEZEL_TEST(imported_record_factory_method_body_shipping_3645,
           imported_class_without_record_returning_method_asan_no_leak) {
    auto dir = wtDir3645();
    LibHandle libH = buildLib(dir, kGenericLibSrc3645);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    AppHandle appH = buildApp(dir, libH.objPath, kGenericAppSrc3645,
                              /*sanitize=*/true);
    BEZEL_EXPECT_TRUE(appH.ok);
    if (!appH.ok) return;
    RunResult rr = runCapture(appH.exePath);
    // The program returns `s.Length` where `s = "no-record-return"` (16
    // chars). The local-twin of this row is admitted through the
    // importer as before; no leak either way.
    BEZEL_EXPECT_TRUE(rr.code == 16);
    BEZEL_EXPECT_TRUE(rr.out.find("LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(rr.out.find("Direct leak") == std::string::npos);
    BEZEL_EXPECT_TRUE(rr.out.find("Indirect leak") == std::string::npos);
}

}  // namespace bezel::codegen
