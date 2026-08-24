// Regression tests for #3187 (§28.3 / §29.8 / §29.11 / §2.8.3 rule 4, v0.254) —
// two measured defects on one cross-module shape: the §28.3 "Deferred-activation
// callees" rule never fired for an imported callee, and an imported iterator
// yielded NOTHING.
//
// SPEC AUTHORITY (v0.254):
//   §28.3 — "Deferred-activation callees": an unbound owned temporary passed as
//            a plain argument to such a callee is admissible only when the
//            returned frame is itself fully consumed within the same full
//            expression; otherwise rejected. The callee classification is by
//            declared return type: a §29.3/§30.7 iterator/async-iterator
//            interface, a §29.11 lazy-query sentinel, or a Task-family type.
//   §29.8 — iterator frame's source borrows live until the frame is disposed.
//   §29.11 — enumerator vtable slots (MoveNext, Current, Dispose).
//   §2.8.3 rule 4 — imported symbols are observationally equivalent to
//            same-module symbols; a defect class that fires locally must fire
//            identically across a module boundary.
//
// THE DEFECTS (this commit pins BOTH; per-fix review flags a partial fix).
//
//   (a) The §28.3 gate (Ownership_Checks.cpp:9287
//   `isDeferredActivationCallResult`). The pre-fix implementation required an
//   AST node AND a single-part name; that AST-only lookup silently classified
//   every imported `IEnumerable<T>` / `IAsyncEnumerable<T>` / Task-family call
//   result as "not a deferred-activation call". So §28.3's reject was dead
//   across a module boundary — the silent soundness widening the issue pins.
//   The fix widens the head-name lookup across all carrying channels the
//   TypeRef may be represented by — AST, `importedName`, declaration-side
//   `name` (Builtin-only) — mirroring `isTaskFamily`'s
//   (checkBorrowingTaskArrayLiteral, same file below) cross-channel lookup
//   for the same defect class.
//
//   (b) The imported iterator yields nothing (LlvmBackend_Stmts.cpp:6700+,
//   `lowerIEnumerableBuiltinFor`'s `checkPureIEnumerable`). The pre-fix
//   `rt->astNode` guard excluded every TypeRef reconstructed from cross-module
//   metadata (no AST exists for an imported builtin interface). An imported
//   `IEnumerable<T>`-typed local holding a yield-iterator result was then
//   classified as array-backed; the `bz_array_len` read landed at the
//   iterator frame's state-index offset (= 8, after the vtable pointer),
//   initially 0 → `0 < 0` false, body never runs, exit 0 (the audit's
//   hypothesis confirmed by direct probe: "store nothing into the loop
//   variable so the loop spins on a constant" — the constant is the state
//   index, interpreted as `len`).
//
// COVERAGE (6 rows):
//   Row 1 — POSITIVE (cross-module (a)+bound first): the dangerous shape with
//           the source named first is CLEAN (the §28.3 remedy verbatim).
//   Row 2 — POSITIVE (cross-module (a)+dangerous): the dangerous shape with
//           an unbound owned temporary is REJECTED. Before the fix: silently
//           accepted (the §28.3 rule was dead cross-module). After: rejected.
//           Pins (a).
//   Row 3 — POSITIVE (cross-module (b)): an imported iterator bound to a
//           local, then iterated, ENUMERATES. Before the fix: yields nothing,
//           exit 0 (the (b) bug). After: returns 3. Pins (b).
//   Row 4 — REGRESSION GUARD (single-module (a)): the local twin of Row 2
//           continues to REJECT (the §28.3 rule was always live locally;
//           the fix must not break the local case).
//   Row 5 — REGRESSION GUARD (single-module (b)): the local twin of Row 3
//           continues to enumerate and return 3.
//   Row 6 — OVER-REJECTION CONTROL: a non-deferred-activation callee with
//           an unbound owned temporary argument is NOT flagged. Cross-module
//           parity.

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
// (#3002-class COVERAGE SHORT). popen() usage is kept (per-stem uniqueness
// already mitigates the overwrite race; full fork isolation is a separate
// concern).
const std::filesystem::path& wtDir3187() {
    return wtScratchDir("wt-3187");
}

struct TempFile {
  std::string path;
  explicit TempFile(std::string p) : path(std::move(p)) {}
  ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
  std::ofstream out(path);
  out << src;
}

// Compile lib → object, then compile the importer that consumes the lib via
// `Driver::compileFile`, link with the lib object, and run. Returns:
//   * the program's exit code on a clean run,
//   * -1 on a compile or link failure,
//   * -2 if the toolchain is unavailable.
int compileAndRunCrossModule(const std::string& libSrc,
                             const std::string& mainSrc) {
  auto dir = wtDir3187();
  std::string stem = unique_stem("bezel_xmod3187");
  TempFile lSrc((dir / (stem + "_lib.bzl")).string());
  TempFile lObj((dir / (stem + "_lib.o")).string());
  TempFile mSrc((dir / (stem + "_main.bzl")).string());
  TempFile mExe((dir / stem).string());

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
  std::remove(mExe.path.c_str());
  if (status == -1 || !WIFEXITED(status)) return -1;
  return WEXITSTATUS(status);
}

// Compile-only cross-module variant. Returns:
//   * 1 if the compile+link succeeded (silent accept — the (a) reject
//     would have produced 0),
//   * 0 if the compile failed (the (a) reject fired),
//   * -1 on a toolchain failure.
int compileOnlyCrossModule(const std::string& libSrc,
                           const std::string& mainSrc) {
  auto dir = wtDir3187();
  std::string stem = unique_stem("bezel_xmod3187_co");
  TempFile lSrc((dir / (stem + "_lib.bzl")).string());
  TempFile lObj((dir / (stem + "_lib.o")).string());
  TempFile mSrc((dir / (stem + "_main.bzl")).string());
  TempFile mExe((dir / stem).string());

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
  std::remove(mExe.path.c_str());
  if (rm.success) return 1;
  return 0;
}

// Single-module compile-only. Returns:
//   * 1 if the compile+link succeeded (the shape was admitted),
//   * 0 if the compile failed (the rule under test rejected),
//   * -1 on a toolchain failure.
int compileOnlySingleModule(const std::string& src) {
  auto dir = wtDir3187();
  std::string stem = unique_stem("bezel_xmod3187_sm");
  TempFile sSrc((dir / (stem + ".bzl")).string());
  TempFile sExe((dir / stem).string());
  writeSource(sSrc.path, src);
  bezel::Driver driver;
  bezel::CompileResult r = driver.compileFile(sSrc.path, sExe.path);
  std::remove(sExe.path.c_str());
  if (r.success) return 1;
  return 0;
}

// Single-module compile-and-run. Returns the exit code or -1 on failure.
int compileAndRunSingleModule(const std::string& src) {
  auto dir = wtDir3187();
  std::string stem = unique_stem("bezel_xmod3187_sm");
  TempFile sSrc((dir / (stem + ".bzl")).string());
  TempFile sExe((dir / stem).string());
  writeSource(sSrc.path, src);
  bezel::Driver driver;
  bezel::CompileResult r = driver.compileFile(sSrc.path, sExe.path);
  if (!r.success) return -1;
  int status = std::system(sExe.path.c_str());
  std::remove(sExe.path.c_str());
  if (status == -1 || !WIFEXITED(status)) return -1;
  return WEXITSTATUS(status);
}

// The library used by every cross-module row. A disposable class SrcD (whose
// `new SrcD(3)` produces an unbound owned temporary — the §28.3 source shape),
// and a static iterator factory `GenD.Nums(s)` that yields `s.N` once and
// returns. Mirrors the issue's `da.lib.bzl` repro.
const char* kLib3187 =
    "module DaLib3187; "
    "public class SrcD "
    "  public property N: int := 0; "
    "  public constructor(n: int) N := n; end constructor; "
    "end SrcD; "
    "public class GenD "
    "  public static method Nums(s: SrcD): IEnumerable<int> "
    "    yield return s.N; "
    "  end Nums; "
    "end GenD;";

// The single-module twin used by Rows 4 and 5: one source with everything
// inlined. The fix must not break the local case (Row 4) and must not
// regress the already-working local twin (Row 5).
const char* kSingleModule3187 =
    "module DaSingle3187; "
    "public class SrcD "
    "  public property N: int := 0; "
    "  public constructor(n: int) N := n; end constructor; "
    "end SrcD; "
    "public class GenD "
    "  public static method Nums(s: SrcD): IEnumerable<int> "
    "    yield return s.N; "
    "  end Nums; "
    "end GenD; "
    "public class AppDa "
    "  public static method Main(): int "
    "    var e := GenD.Nums(new SrcD(3)); "
    "    for var x in e do "
    "      return x; "
    "    end for; "
    "    return -1; "
    "  end Main; "
    "end AppDa;";

}  // namespace

// ---------------------------------------------------------------------------
// Row 1 — POSITIVE (cross-module (a)+bound first): the dangerous shape with
// the source named first is CLEAN (the §28.3 remedy verbatim).
// ---------------------------------------------------------------------------
BEZEL_TEST(xmod_deferred_activation_3187,
           cross_module_deferred_activation_bound_source_clean_spec_28_3) {
  int rc = compileOnlyCrossModule(
      kLib3187,
      "module DaApp; import DaLib3187; "
      "public class AppDa "
      "  public static method Main(): int "
      "    using var src := new SrcD(3); "
      "    var e := GenD.Nums(src); "
      "    for var x in e do "
      "      return x; "
      "    end for; "
      "    return -1; "
      "  end Main; "
      "end AppDa;");
  // rc == 1 (compile succeeded) is the success of the clean shape.
  BEZEL_EXPECT_EQ(rc, 1);
}

// ---------------------------------------------------------------------------
// Row 2 — POSITIVE (cross-module (a)+dangerous): the dangerous unbound-
// temporary escape is REJECTED at the cross-module call site. Before this
// fix: SILENTLY ACCEPTED (the §28.3 reject was dead cross-module). After:
// rejected. Pins Shape (a).
// ---------------------------------------------------------------------------
BEZEL_TEST(xmod_deferred_activation_3187,
           cross_module_deferred_activation_unbound_rejected_spec_28_3) {
  int rc = compileOnlyCrossModule(
      kLib3187,
      "module DaApp; import DaLib3187; "
      "public class AppDa "
      "  public static method Main(): int "
      "    var e := GenD.Nums(new SrcD(3)); "
      "    for var x in e do "
      "      return x; "
      "    end for; "
      "    return -1; "
      "  end Main; "
      "end AppDa;");
  // rc == 0 (compile failed) is the success of the (a)-reject pin.
  BEZEL_EXPECT_EQ(rc, 0);
}

// ---------------------------------------------------------------------------
// Row 3 — POSITIVE (cross-module (b)): an imported iterator bound to a
// local, then iterated, ENUMERATES. Before the fix: yields nothing, exit 0
// (the (b) bug — confirmed via probe on HEAD before the fix). After: 3
// (the single yield from `GenD.Nums(s.N)`). Pins Shape (b).
//
// Note: the source is BOUND FIRST (the §28.3 remedy) — the dangerous
// unbound-temporary shape is the (a)-reject pin (Row 2) and would compile-
// fail before even reaching codegen. Row 3 verifies that, once the source
// is bound, the imported iterator enumerates correctly cross-module.
// ---------------------------------------------------------------------------
BEZEL_TEST(xmod_deferred_activation_3187,
           cross_module_iter_local_enumerates_spec_29_2) {
  int rc = compileAndRunCrossModule(
      kLib3187,
      "module DaApp; import DaLib3187; "
      "public class AppDa "
      "  public static method Main(): int "
      "    using var src := new SrcD(3); "
      "    var e := GenD.Nums(src); "
      "    var total := 0; "
      "    for var x in e do "
      "      total := total + x; "
      "    end for; "
      "    return total; "
      "  end Main; "
      "end AppDa;");
  // rc == 3 — the (b) bug returned 0 before the fix.
  BEZEL_EXPECT_EQ(rc, 3);
}

// ---------------------------------------------------------------------------
// Row 4 — REGRESSION GUARD (single-module (a)): the local twin of Row 2 must
// continue to REJECT (the §28.3 rule was always live locally; the fix must
// not break the local case).
// ---------------------------------------------------------------------------
BEZEL_TEST(xmod_deferred_activation_3187,
           single_module_deferred_activation_unbound_rejected_spec_28_3) {
  // Single module — same shape as Row 2's cross-module dangerous escape.
  int rc = compileOnlySingleModule(
      "module M3187SmA; "
      "public class SrcD "
      "  public property N: int := 0; "
      "  public constructor(n: int) N := n; end constructor; "
      "end SrcD; "
      "public class GenD "
      "  public static method Nums(s: SrcD): IEnumerable<int> "
      "    yield return s.N; "
      "  end Nums; "
      "end GenD; "
      "public class AppDa "
      "  public static method Main(): int "
      "    var e := GenD.Nums(new SrcD(3)); "
      "    for var x in e do "
      "      return x; "
      "    end for; "
      "    return -1; "
      "  end Main; "
      "end AppDa;");
  BEZEL_EXPECT_EQ(rc, 0);
}

// ---------------------------------------------------------------------------
// Row 5 — REGRESSION GUARD (single-module (b)): the local twin of Row 3 must
// continue to enumerate and return 3. The local case never had the (b) bug;
// the fix must not regress it.
//
// Note: the source is BOUND FIRST (mirroring Row 3) so the test is the
// true regression guard for the (b) codegen widening, decoupled from the
// §28.3 (a) reject (Row 4).
// ---------------------------------------------------------------------------
BEZEL_TEST(xmod_deferred_activation_3187,
           single_module_iter_local_enumerates_spec_29_2) {
  int rc = compileAndRunSingleModule(
      "module M3187SmB; "
      "public class SrcD "
      "  public property N: int := 0; "
      "  public constructor(n: int) N := n; end constructor; "
      "end SrcD; "
      "public class GenD "
      "  public static method Nums(s: SrcD): IEnumerable<int> "
      "    yield return s.N; "
      "  end Nums; "
      "end GenD; "
      "public class AppDa "
      "  public static method Main(): int "
      "    using var src := new SrcD(3); "
      "    var e := GenD.Nums(src); "
      "    var total := 0; "
      "    for var x in e do "
      "      total := total + x; "
      "    end for; "
      "    return total; "
      "  end Main; "
      "end AppDa;");
  BEZEL_EXPECT_EQ(rc, 3);
}

// ---------------------------------------------------------------------------
// Row 6 — OVER-REJECTION CONTROL: a non-deferred-activation callee with an
// unbound owned temporary argument is NOT flagged (it is the §28.3 base
// rule's case — the temporary is consumed at the full expression boundary,
// not the deferred-activation reject). Cross-module parity for this control.
// ---------------------------------------------------------------------------
BEZEL_TEST(xmod_deferred_activation_3187,
           cross_module_non_deferred_callee_unbound_clean_spec_28_3) {
  // Lib: a non-deferred-activation callee (returns `int`, not one of the
  // iterator/lazy-query/Task shapes) that takes a SrcD by value.
  const char* kLib3187Plain =
      "module DaLib3187Plain; "
      "public class SrcD "
      "  public property N: int := 0; "
      "  public constructor(n: int) N := n; end constructor; "
      "end SrcD; "
      "public class Program "
      "  public static method Peek(s: SrcD): int => s.N; "
      "end Program;";
  int rc = compileOnlyCrossModule(
      kLib3187Plain,
      "module DaApp; import DaLib3187Plain; "
      "public class AppDa "
      "  public static method Main(): int "
      "    return Program.Peek(new SrcD(3)); "
      "  end Main; "
      "end AppDa;");
  // rc == 1 — the deferred-activation reject must NOT fire on a non-
  // deferred-activation callee.
  BEZEL_EXPECT_EQ(rc, 1);
}
