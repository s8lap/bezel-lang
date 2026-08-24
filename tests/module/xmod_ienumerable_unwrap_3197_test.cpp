// Regression tests for #3197 — an IMPORTED IEnumerable<T> was handed back
// UNWRAPPED by `wrapUserClassAsIEnumerable`, driving the §29.11 enumerator
// vtable through a raw class pointer (SIGSEGV 139 on the standard
// `for-in` shape). Per the in-repo handoff
// (docs/proposals/imported-symbol-programme-status-2026-08-14.md §6b):
//
//   "The accessor-slot half is #3190; this is the enumerator half, which
//    is a different resolution mechanism and is filed separately for that
//    reason."
//
// SPEC AUTHORITY (v0.254):
//   §29.1 — `IEnumerable<T>.GetEnumerator()` returns an owned enumerator.
//   §29.2 — `for` desugars via `__source.GetEnumerator(); MoveNext();
//            Current` (or `TakeCurrent` on the owning path).
//   §29.11 — enumerator vtable slots: MoveNext + Current + Dispose.
//   §2.8.2 — artifact interface is a semantic requirement; an imported
//            user-class `IEnumerable<T>` conformance must drive the
//            enumerator vtable exactly as the same-module twin does.
//
// THE DEFECT (Shape A in the issue, silent SIGSEGV). `wrapUserClassAsIEnumerable`
// in src/codegen/LlvmBackend_Assign.cpp took `getEnumSym->decl` (an AST
// MethodDecl) and bailed to `return src` when the cast was null — which it
// always is for an imported MethodSymbol (MemberSymbol::decl is null for
// imports; the loader does not re-parse body text for non-generic classes).
// The structural `MoveNext` + `Current` scan iterates `ctx_.symbols()`, which
// by construction excludes imported symbols (makeImportedSymbol keeps them
// out). And the `Current` property read had no `ast::PropertyDecl` for an
// imported enumerator. Net result: the raw class instance was returned and
// driven through the §29.11 enumerator vtable — exit 139.
//
// THE FIX (this commit). Three sites, all in the enumerator-DISCOVERY path:
//
//   1. `wrapUserClassAsIEnumerable`: when `getEnumDecl` is null (imported),
//      take the return type from `ctx_.importedReturnType(getEnumSym)`.
//   2. The structural `MoveNext` + `Current` scan walks `importedSymbols()`
//      too. The `methodFns_.count(ms)` gate already answers correctly for
//      an imported method (declareImportedFunctions populates it).
//   3. The `Current` property read uses `importedPropGetterFns_[ps]` when
//      the property has no local decl. `declareImportedPropertyGetters`
//      populates it for the non-virtual, non-interface shapes — and the
//      `Current` here is non-virtual and class-parented, so neither of its
//      two skips applies.
//
// The corresponding fixes in `lowerIEnumerableFor` (LlvmBackend_Stmts.cpp)
// handle the direct `for-in` spelling. After this fix the loud NYI at
// Stmts.cpp's `lowerIEnumerableFor` early-return (`if (!getEnumDecl || …)
// return false;`) — which the issue notes is reached BEFORE the `Current`-
// getter site at ~L7411 — runs through the same enumerator-discovery path
// as `wrapUserClassAsIEnumerable`, so the structural scan needs the same
// extension.
//
// COVERAGE (3 rows).
//   Row 1 — twin POSITIVE: an imported class implementing
//   `IEnumerable<int>` whose `Sum` is invoked across a module boundary must
//   iterate the same way the single-module twin does. Before the fix:
//   exit 139. After: 10 (1+2+3+4). Pins Shape A in the issue.
//   Row 2 — twin POSITIVE (same-module control): the character-identical
//   single-module twin returns 10 today and must continue to. Pins that the
//   imported-shape fix does not regress the local twin.
//   Row 3 — over-rejection control: a method declared to return
//   `IEnumerable<int>` whose body returns a non-IEnumerable value (here:
//   `int`) must still be REJECTED. The shape was admitted before only
//   because the unwrap-path's silent fall-through swallowed the
//   type-mismatch at codegen time; after the fix the structural discovery
//   can find an enumerator only when one genuinely exists, so a non-
//   enumerable return is not silently accepted. Confirms the fix narrows
//   the path rather than widening it (per the project's "do not
//   silently accept" rule, #3152).

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace {
namespace fs = std::filesystem;

// Per-process unique working dir under the shared scratch base. The previous
// fs::temp_directory_path() (== /tmp tmpfs) + non-atomic counter pattern was
// race-prone: parallel shards overwrote each other's _lib.bzl / _lib.o /
// _main.bzl mid-compileObject and garbled the in-flight child read. The atomic
// unique_stem + /var/tmp/bezel-scratch parent fixes that class of flake
// (#3002-class COVERAGE SHORT).
const std::filesystem::path& wtDir3197() {
    return wtScratchDir("wt-3197");
}

struct TempFile {
  std::string path;
  explicit TempFile(const std::string& p) : path(p) {}
  ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
  std::ofstream out(path);
  out << src;
}

// Compile lib → object, compile main importing lib → executable, run.
// Returns the exit code, or -1 on compile/link failure. A 139 here is the
// SHAPE-A SIGSEGV the issue pins — the row asserts it is gone.
int compileAndRunCrossModule(const std::string& libSrc,
                             const std::string& mainSrc) {
  auto dir = wtDir3197();
  std::string stem = unique_stem("bezel_xmod3197");
  TempFile lSrc((dir / (stem + "_lib.bzl")).string());
  TempFile lObj((dir / (stem + "_lib.o")).string());
  TempFile mSrc((dir / (stem + "_main.bzl")).string());
  TempFile mExe((dir / (stem + "_main")).string());

  writeSource(lSrc.path, libSrc);
  writeSource(mSrc.path, mainSrc);

  bezel::Driver driver;
  bezel::CompileResult rl = driver.compileObject(lSrc.path, lObj.path);
  if (!rl.success) return -1;

  bezel::Dependency dep;
  dep.metadataPath = lObj.path;
  dep.objectPath   = lObj.path;
  std::vector<bezel::Dependency> deps{dep};

  bezel::CompileResult rm = driver.compileFile(mSrc.path, mExe.path, deps);
  if (!rm.success) return -1;

  int status = std::system(mExe.path.c_str());
  if (status == -1 || !WIFEXITED(status)) return -1;
  return WEXITSTATUS(status);
}

// Single-module helper for the local-twin row.
int compileAndRunSingleModule(const std::string& src) {
  auto dir = wtDir3197();
  std::string stem = unique_stem("bezel_xmod3197_sm");
  TempFile srcF((dir / (stem + ".bzl")).string());
  TempFile exeF((dir / stem).string());

  writeSource(srcF.path, src);

  bezel::Driver driver;
  bezel::CompileResult r = driver.compileFile(srcF.path, exeF.path);
  if (!r.success) return -1;

  int status = std::system(exeF.path.c_str());
  if (status == -1 || !WIFEXITED(status)) return -1;
  return WEXITSTATUS(status);
}

// Returns 1 if compile succeeded (positive control: the test expects a
// REJECT, so 1 means "compiled — the test fails"). Returns 0 on a compile
// failure (the reject happened — the test passes). Mirrors the project's
// "test the boundary" discipline (over-rejection control).
int compileOnly(const std::string& src) {
  auto dir = wtDir3197();
  std::string stem = unique_stem("bezel_xmod3197_co");
  TempFile srcF((dir / (stem + ".bzl")).string());
  TempFile exeF((dir / stem).string());

  writeSource(srcF.path, src);

  bezel::Driver driver;
  bezel::CompileResult r = driver.compileFile(srcF.path, exeF.path);
  return r.success ? 1 : 0;
}

// The lib used by Row 1. An IntRangeEnum (the enumerator) is paired with a
// Coll (the IEnumerable<int> implementor whose GetEnumerator() mints a new
// IntRangeEnum). Mirrors the issue's Shape A repro.
const char* kLib3197 =
    "module Lib3197; "
    "public class IntRangeEnum3197: IEnumerator<int> "
    "  protected cur: int; "
    "  protected hi: int; "
    "  public constructor(lo: int, hi: int) "
    "    cur := lo - 1; "
    "    this.hi := hi; "
    "  end constructor; "
    "  public method MoveNext(): bool "
    "    cur := cur + 1; "
    "    return cur <= hi; "
    "  end MoveNext; "
    "  public property Current: int get => cur; end Current; "
    "end IntRangeEnum3197; "
    "public class Coll3197: IEnumerable<int> "
    "  public method GetEnumerator(): IEnumerator<int> "
    "    return new IntRangeEnum3197(1, 4); "
    "  end GetEnumerator; "
    "end Coll3197;";

// Returns the importer program with a body that iterates a Coll3197 passed
// in as the parameter. Same shape as the issue's app.bzl `C.Sum`.
std::string app3197(const std::string& body) {
  return "module App3197; import Lib3197; "
         "public class C "
         "  public static method Sum(xs: IEnumerable<int>): int "
         "    var t := 0; "
         "    for var x in xs do t := t + x; end; "
         "    return t; "
         "  end Sum; "
         "  public static method " + body +
         "  end Main; "
         "end C; ";
}
}  // namespace

// ---------------------------------------------------------------------------
// Row 1 — POSITIVE (cross-module IEnumerable<int> consumed by for-in)
// §29.1 / §29.2 / §29.11 / §2.8.2 (v0.254) — Shape A of the issue.
//
// Before the fix: SIGSEGV (139) at the §29.11 enumerator vtable dispatch on
// the raw class instance. After: 10 (1+2+3+4).
// ---------------------------------------------------------------------------
BEZEL_TEST(xmod_ienumerable_unwrap_3197, imported_ienumerable_sum_spec_29_2) {
  BEZEL_EXPECT_EQ(compileAndRunCrossModule(kLib3197,
    app3197("Main(): int "
            "    using var c := new Coll3197(); "
            "    return C.Sum(c); ")),
    10);
}

// ---------------------------------------------------------------------------
// Row 2 — POSITIVE CONTROL (character-identical single-module twin).
// §29.2 (v0.254). Must continue to return 10 after the imported-path fix.
//
// This is the same shape Row 1 would compile to if the module boundary were
// absent. The project discipline is that every cross-module fix must ship
// with its single-module twin — and that the twin must remain green — so a
// regression here catches an over-eager narrowing that broke the local
// case (the same defect class as #3191 / #3194 in Wave A of the imported-
// symbol programme).
// ---------------------------------------------------------------------------
BEZEL_TEST(xmod_ienumerable_unwrap_3197, single_module_twin_sum_spec_29_2) {
  BEZEL_EXPECT_EQ(compileAndRunSingleModule(
    "module M3197; "
    "public class IntRangeEnum3197T: IEnumerator<int> "
    "  protected cur: int; "
    "  protected hi: int; "
    "  public constructor(lo: int, hi: int) "
    "    cur := lo - 1; "
    "    this.hi := hi; "
    "  end constructor; "
    "  public method MoveNext(): bool "
    "    cur := cur + 1; "
    "    return cur <= hi; "
    "  end MoveNext; "
    "  public property Current: int get => cur; end Current; "
    "end IntRangeEnum3197T; "
    "public class Coll3197T: IEnumerable<int> "
    "  public method GetEnumerator(): IEnumerator<int> "
    "    return new IntRangeEnum3197T(1, 4); "
    "  end GetEnumerator; "
    "end Coll3197T; "
    "public class C "
    "  public static method Sum(xs: IEnumerable<int>): int "
    "    var t := 0; "
    "    for var x in xs do t := t + x; end; "
    "    return t; "
    "  end Sum; "
    "  public static method Main(): int "
    "    using var c := new Coll3197T(); "
    "    return C.Sum(c); "
    "  end Main; "
    "end C;"),
    10);
}

// ---------------------------------------------------------------------------
// Row 3 — OVER-REJECTION CONTROL (a method declared IEnumerable<int> whose
// body returns a non-enumerable).
//
// Before this fix, `wrapUserClassAsIEnumerable` and `lowerIEnumerableFor`
// returned the raw class pointer silently when the structural enumerator
// scan found nothing — and any caller of that method then drove the
// §29.11 vtable against a non-IEnumerable object. The shape below is one
// such would-be-silent-accept: a method declared to return IEnumerable<int>
// whose body returns an `int`. A correct implementation REJECTS this
// (return type mismatch — §11.2). If the fix accidentally widened the path
// (e.g., by treating null-`getEnumDecl` as "any return type accepted"),
// this test would compile and fail at runtime.
//
// This row is the project's over-rejection-control guard (programme
// handoff §4 rule 5): a test set that would pass if the implementation
// accepted everything is not a test set.
// ---------------------------------------------------------------------------
BEZEL_TEST(xmod_ienumerable_unwrap_3197, non_enumerable_return_rejected_spec_11_2) {
  // Expect: compile failure (return-type mismatch). compileOnly returns 0
  // on failure, 1 on success. The test asserts 0.
  BEZEL_EXPECT_EQ(compileOnly(
    "module M3197Rej; "
    "public class C "
    "  public static method BadReturn(): IEnumerable<int> "
    "    return 42; "
    "  end BadReturn; "
    "  public static method Main(): int "
    "    return 0; "
    "  end Main; "
    "end C;"),
    0);
}
