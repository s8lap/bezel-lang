// Cross-module exception propagation — fix #492.
//
// §31.1 / §31.2 / §2.6: a `throw` in a separately-compiled library module must
// propagate to the importing module's exception machinery. Before the fix, each
// separately-compiled module defined its OWN `bz$exc` global (BSS, InternalLinkage)
// so the library's throw wrote the library's copy while the importer's checks read
// a still-null copy — the exception silently disappeared and the call returned 0.
//
// The fix makes `bz$exc` a single shared symbol defined in the runtime
// (bezel_runtime.cpp) and referenced via an external declaration in every module's
// codegen, so all modules read/write the same global.
//
// Tests (spec-derived, per-milestone discipline):
//   (a) cross-module throw UNCAUGHT → exit 70 (runtime's bz_unhandled_exception)
//   (b) cross-module throw CAUGHT by importer's try/catch → handler runs (exit N)
//   (c) single-module throw UNCAUGHT → exit 70 (control: must still work)
//   (d) single-module throw CAUGHT → handler runs (control)
//   (e) cross-module throw of a user-defined exception type, CAUGHT, with a field
//       value that is readable in the catch arm

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>
#include <vector>


namespace bezel::module {

using bezel::test::JitResult;

namespace {

// The runtime's abnormal-termination exit code (EX_SOFTWARE, BSD sysexits.h).
// §33.3 / §22.13: uncaught exception escaping Main → bz_unhandled_exception() →
// exit(70).
constexpr int kUnhandledExit = 70;

// ── HARNESS (test #6386) ───────────────────────────────────────────────────
//
// This file used to carry its own cross-module runner (`buildAndRun`: a
// `compileObject`, a one-entry `Dependency` vector, `std::system`, and a
// hand-rolled -1 returned for a compile failure, a link failure, a fork failure
// and a runtime signal alike) plus a single-module twin. Both are now the
// shared helpers from tests/support/aot_support.hpp (#6379): `runAotModules` for
// the cross-module rows and `runAot` for the two controls.
//
// `runAotModules`' run half is `aot_detail::runProgramFill` — the SAME function
// `runAot` calls — and its compile halves are `forkedCompileModule` (a
// `Driver::compileObject`, the only shape that embeds the §2.4 `.bezel.meta`
// public interface) and `forkedCompileWithDeps`. A driver crash in the library
// build is therefore FORK-ISOLATED, where the old runner ran it in the test
// process.
//
// THE LIBRARY MODULE NAME IS A PARAMETER, not a constant: these rows each name
// their library differently (`LibThrow`, `LibCatch`, `LibField`, `ELib`) and the
// entry is `Main` in all of them. The harness matches a module to its `import`
// by NAME, so a wrong name here would make every row report the same unrelated
// "module not found" reject — a port that goes green by breaking all its rows
// at once. That is why the name travels with the source.
//
// ⚠ WHAT THE PORT ADDS. Each row asserted a bare exit code against a runner
// whose -1 could not say WHICH failure it was. Each now asserts `r.ran` FIRST
// and unconditionally, then the SAME exit code as before — 70, 55, 70, 77, 33,
// 55, 11, 70. No value assertion changed.
//
// ⚠ THE ENTRY POINT IS WORTH NAMING. Three of these rows assert
// `kUnhandledExit` (70) — the runtime's abnormal-termination code for an
// exception escaping Main. Under the old runner a LINK failure also produced
// -1, and -1 is nowhere near 70, so these rows were already safe against it.
// What the port adds is that a re-break reports WHY: `r.ran` false with the
// driver stderr (or clang's linker text, or "program terminated by signal N",
// each carrying its own literal) in `r.diagnostics`, instead of a bare number
// mismatch against 70.
//
// ⚠ ONE REAL DIFFERENCE, measured and deliberate. The old runner executed with
// `std::system(exe)`, inheriting the ambient environment, so under the #1822
// whole-suite ASan sweep (`BEZEL_AOT_FORCE_SANITIZE=address`) it ran with
// LeakSanitizer ON by default and a leak would have replaced the program's own
// exit status with LSan's. `runAot` pins `detect_leaks=0` under the sweep
// (#4532 measured that whole-suite leak detection is too noisy, and §28.2
// checked disposal leaks BY DESIGN). MEASURED under that sweep at this commit:
// all eight rows are clean, so no row here depended on the inherited detection.

JitResult buildAndRun(const std::string& libName, const std::string& libSrc,
                      const std::string& mainSrc) {
    return bezel::test::runAotModules(
        {bezel::test::ModuleSource{libName, libSrc},
         bezel::test::ModuleSource{"Main", mainSrc}},
        "Main");
}

// Single-module build for the control tests — `runAot`, not `runAotModules`:
// #6379 is explicit that the multi-module helper is NOT a replacement for it,
// because a one-module row keeps the content-addressed link-cache that a whole
// gate collapses 2-3 compilations of the same program into 1.
JitResult buildAndRunSingle(const std::string& src) {
    return bezel::test::runAot(src);
}

}  // namespace

// ── (c) control: single-module throw UNCAUGHT → exit 70 ─────────────────────
// §22.13 / §31.1: a throw with no handler in Main must exit abnormally.
// This exercises the single-module path to confirm the fix does not regress it.
BEZEL_TEST(cross_module_exception, single_module_throw_uncaught_exits_70_spec_31_1) {
    JitResult r = buildAndRunSingle(
        "module M; "
        "class Boom: Exception end Boom; "
        "class C "
        "  public static method Main(): int "
        "    throw new Boom(); "
        "  end Main; "
        "end C;"
        );
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kUnhandledExit);
}

// ── (d) control: single-module throw CAUGHT → handler runs ──────────────────
// §31.2: a caught throw does not escape; Main completes normally with the
// handler's return value.
BEZEL_TEST(cross_module_exception, single_module_throw_caught_handler_runs_spec_31_2) {
    JitResult r = buildAndRunSingle(
        "module M; "
        "class Boom: Exception end Boom; "
        "class C "
        "  public static method Main(): int "
        "    try "
        "      throw new Boom(); "
        "    catch Boom => return 55; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;"
        );
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 55);
}

// ── (a) cross-module throw UNCAUGHT → exit 70 ───────────────────────────────
// §31.1 / §2.6 / fix #492: AlwaysRaise() throws in the library; Main calls it
// with no enclosing handler. Before the fix, bz$exc was per-module so the throw
// was a no-op and exit 100 was returned (x+100 where x==0). After the fix the
// exception propagates and bz_unhandled_exception terminates with exit 70.
BEZEL_TEST(cross_module_exception, cross_module_throw_uncaught_exits_70_spec_31_1) {
    JitResult r = buildAndRun("LibThrow",
        // lib_throw.bzl
        "module LibThrow; "
        "public class Boom: Exception end Boom; "
        "public class Raiser "
        "  public static method AlwaysRaise(): int "
        "    throw new Boom(); "
        "  end AlwaysRaise; "
        "end Raiser;",
        // main.bzl
        "module Main; "
        "import LibThrow; "
        "class C "
        "  public static method Main(): int "
        "    var x := Raiser.AlwaysRaise(); "
        "    return x + 100; "     // must NOT execute; if it does → exit 100
        "  end Main; "
        "end C;"
        );
    // Before fix: code == 100 (throw was a no-op).
    // After fix : code == kUnhandledExit (70).
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kUnhandledExit);
}

// ── (b) cross-module throw CAUGHT by importer's try/catch ───────────────────
// §31.2 / §2.6 / fix #492: AlwaysRaise() throws in the library; Main wraps the
// call in a try/catch and should observe the exception. Before the fix the catch
// arm never triggered (bz$exc was still null in Main's module) so the "no
// exception" path ran and return 99 was observed. After the fix the catch runs
// and return 77 is observed.
BEZEL_TEST(cross_module_exception, cross_module_throw_caught_in_importer_spec_31_2) {
    JitResult r = buildAndRun("LibCatch",
        // lib.bzl
        "module LibCatch; "
        "public class AppErr: Exception end AppErr; "
        "public class Thrower "
        "  public static method Raise(): int "
        "    throw new AppErr(); "
        "  end Raise; "
        "end Thrower;",
        // main.bzl
        "module Main; "
        "import LibCatch; "
        "class C "
        "  public static method Main(): int "
        "    try "
        "      var dummy := Thrower.Raise(); "
        "      return 99; "       // must NOT execute
        "    catch AppErr => return 77; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;"
        );
    // Before fix: code == 99 (catch never triggered).
    // After fix : code == 77.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 77);
}

// ── (e) cross-module throw of user exception with a message field ─────────────
// §31.1 / §31.3 / fix #492: the thrown exception object carries a field value
// readable in the catch arm. This exercises that the in-flight pointer itself is
// correctly shared (not just a non-null sentinel).
BEZEL_TEST(cross_module_exception, cross_module_throw_with_field_readable_spec_31_3) {
    JitResult r = buildAndRun("LibField",
        // lib.bzl: exception class carries a `code` field
        "module LibField; "
        "public class CodeErr: Exception "
        "  public code: int; "
        "  public constructor(c: int) code := c; end constructor; "
        "  destructor end destructor; "
        "end CodeErr; "
        "public class FieldThrower "
        "  public static method RaiseCode(n: int): int "
        "    throw new CodeErr(n); "
        "  end RaiseCode; "
        "end FieldThrower;",
        // main.bzl
        "module Main; "
        "import LibField; "
        "class C "
        "  public static method Main(): int "
        "    try "
        "      var dummy := FieldThrower.RaiseCode(33); "
        "      return 0; "        // must NOT execute
        "    catch var e: CodeErr => return e.code; "  // should return 33
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;"
        );
    // After fix: code == 33 (the field value from the cross-module exception object).
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 33);
}

// ── (f) fix #805: cross-module throw caught by a BUILT-IN exception type ───────
// §31.2 / §2.5: an exception thrown by an imported module and caught in the
// importer by a BUILT-IN exception type (`catch Exception`) is now matched. The
// catch lowers emitIsA against the built-in type descriptor `btd$Exception`;
// before this fix that descriptor was per-module (InternalLinkage), so the
// importer's `btd$Exception` address never equalled the address embedded in the
// imported exception object's vtable → the catch never fired (uncaught, exit 70).
// LinkOnceODR linkage merges the descriptor to one symbol so the pointer match
// succeeds. (The existing user-type-catch tests above already worked — user
// class descriptors are shared via the import mechanism; built-in descriptors
// were the gap.)
BEZEL_TEST(cross_module_exception, cross_module_throw_caught_by_builtin_Exception_805) {
    JitResult r = buildAndRun("ELib",
        "module ELib; "
        "public class Thrower "
        "  public static method Go(): int => throw new Exception(\"x\"); "
        "end Thrower;",
        "module Main; "
        "import ELib; "
        "class C public static method Main(): int "
        "  try return Thrower.Go(); catch Exception => return 55; end; "
        "end Main; end C;"
        );
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 55);
}

// §31.2 / §12: a DERIVED built-in exception (ArgumentException) thrown
// cross-module is caught as its base `Exception` — the descriptor chain
// (btd$ArgumentException -> btd$Exception, all shared) is walked cross-module.
BEZEL_TEST(cross_module_exception, cross_module_derived_builtin_caught_as_base_805) {
    JitResult r = buildAndRun("ELib",
        "module ELib; "
        "public class T "
        "  public static method ThrowArg(): int => "
        "    throw new ArgumentException(\"a\"); "
        "end T;",
        "module Main; "
        "import ELib; "
        "class C public static method Main(): int "
        "  try return T.ThrowArg(); catch Exception => return 11; end; "
        "end Main; end C;"
        );
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 11);
}

// §31.2 (no false catch): a built-in exception thrown cross-module is NOT caught
// by a non-matching built-in catch type — it propagates uncaught (exit 70). Pins
// that the shared-descriptor change did not make isA over-match.
BEZEL_TEST(cross_module_exception, cross_module_builtin_non_matching_not_caught_805) {
    JitResult r = buildAndRun("ELib",
        "module ELib; "
        "public class T "
        "  public static method ThrowInvalid(): int => "
        "    throw new InvalidOperationException(\"i\"); "
        "end T;",
        "module Main; "
        "import ELib; "
        "class C public static method Main(): int "
        "  try return T.ThrowInvalid(); catch ArgumentException => return 33; end; "
        "end Main; end C;"
        );
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kUnhandledExit);
}

}  // namespace bezel::module
