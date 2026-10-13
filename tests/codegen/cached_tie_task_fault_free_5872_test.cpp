// Fix-forward for #5872 (the rejected eb0f33d3 / f5249570): the §22.7 cached
// TypeInitializationException became a PROCESS-GLOBAL instance, but the commit
// gated only 2 of the 4 terminal sites that can free an exception object. The
// other two are pure-runtime frees with no codegen gate, and a task whose entry
// touches a failed static type hands them the single cached instance.
//
//   Site 1 — bz_domain_spawn's DomainFaultedException fault-wrap frees the
//            original fault exception (bezel_runtime_concurrency.cpp:3739).
//   Site 2 — bz_scope_dispose_exc frees the first-fault-wins SWALLOW of a later
//            scope.Spawn child's errObj (:4000, reached from :4242).
//
// Pre-fix both are deterministic heap-use-after-free (ASan, measured 3/3):
// bz$async$…Resume reads the block excGV still points at.
//
// §22.7 (v0.334): "The instance constructed on first failure is the one cached
// and rethrown; the triggering access and every subsequent access see the same
// cached TypeInitializationException instance."
// §31.4 (v0.334): "The cached TypeInitializationException and its InnerException
// are runtime-owned for the life of the process: they are exempt from §31.1's
// dispose-after-arm rule — the runtime does not dispose the cached instance when
// a catch arm completes, which is what makes re-raising the same instance on
// every subsequent touch of the failed type sound."
//
// ORACLE. These are ASan rows, not exit-code rows: the signal is the sanitizer
// firing (runUnderAsan returns 1) versus clean (0). The program's own exit code
// is asserted too, and it is what proves the row is NOT VACUOUS — the catch arm
// must actually run (55), not fall through to `return 0`. A program that never
// touched the failed type would exit 0 and ASan would never fire, so a green
// row genuinely means "the cached instance survived every free on its path".
//
// detect_leaks=0 on the UAF rows, per the convention noted at asanProbe below.

#include "support/aot_support.hpp"        // runAot
#include "support/codegen_support.hpp"
#include "support/run_under_asan.hpp"     // runUnderAsan
#include "test_harness.hpp"
#include "native_test_harness.hpp"

using bezel::test::runAot;

namespace {

// ASan probe: 0 = clean; 1 = ASan/LSan fired; negative = infra unavailable
// (SKIP, not a green — the caller asserts on it rather than swallowing it).
// detect_leaks=0 by default, same convention as
// async_618_domain_serial_test.cpp: the main domain's permanent ref is
// deliberately never released inside the sub-process, so LSan would flag
// infrastructure rather than the thing under test.
int asanProbe(const std::string& source, const char* tag) {
    return bezel::test::runUnderAsan(source, tag, /*detectLeaks=*/false);
}

// Leak-detecting variant, for the differential control: an exception the fix
// wrongly stops disposing is reported as a LEAK, so that control needs leaks ON.
int asanProbeLeaky(const std::string& source, const char* tag) {
    return bezel::test::runUnderAsan(source, tag, /*detectLeaks=*/true);
}

// A type permanently in the §22.7 failed state. Peek() gives the spawned entry
// a plain void method that touches the type (a Domain.Spawn entry returning
// Task<int> trips the §30.14 sendable gate, so the entry cannot carry a result).
const char* kFailedType =
    "class FS "
    "  public static _v: int := Throw(); "
    "  private static method Throw(): int "
    "    throw new ArgumentException(\"inner-msg\"); return 0; "
    "  end Throw; "
    "  public static method Peek(): int "
    "    return _v; "
    "  end Peek; "
    "end FS; ";

}  // namespace

// PROBE D / SITE 1 — a Domain.Spawn whose entry touches the failed type. The
// child domain's entry_cont wraps the cached TIE in a DomainFaultedException
// and, pre-fix, freed the original: the block excGV still points at. Main's
// later touch then re-reads freed heap.
BEZEL_TEST(cached_tie_task_fault_free_5872, domain_spawn_fault_wrap_keeps_cached_tie) {
    BEZEL_EXPECT_EQ(
        asanProbe(std::string("module M; ") + kFailedType +
                  "class Entry "
                  "  public static method Run() "
                  "    FS.Peek(); "
                  "  end Run; "
                  "end Entry; "
                  "class C "
                  "  public static async method Main(): Task<int> "
                  "    try await Domain.Spawn(Entry.Run); "
                  "    catch var e: Exception => do end end; "
                  "    try _ := FS._v; "
                  "    catch var ex: TypeInitializationException => do return 55; end end; "
                  "    return 0; "
                  "  end Main; "
                  "end C;",
                  "bezel_5872_domain_spawn"),
        0);
}

// Same program through the plain AOT oracle, asserting the catch arm actually
// ran. Without the fix this is a heap-use-after-free; with it, 55.
BEZEL_TEST(cached_tie_task_fault_free_5872, domain_spawn_catch_arm_still_receives_tie) {
    auto r = runAot(std::string("module M; ") + kFailedType +
                    "class Entry "
                    "  public static method Run() "
                    "    FS.Peek(); "
                    "  end Run; "
                    "end Entry; "
                    "class C "
                    "  public static async method Main(): Task<int> "
                    "    try await Domain.Spawn(Entry.Run); "
                    "    catch var e: Exception => do end end; "
                    "    try _ := FS._v; "
                    "    catch var ex: TypeInitializationException => do return 55; end end; "
                    "    return 0; "
                    "  end Main; "
                    "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 55);
}

// PROBE G / SITE 2 — two scope.Spawn children both touch the failed type. The
// scope's fault selection is first-fault-wins, so the SECOND child's errObj is
// swallowed and freed at bz_scope_dispose_exc (:4242 -> :4000). That errObj is
// the one cached instance, so pre-fix the scope freed it and Main's later touch
// re-read freed heap.
BEZEL_TEST(cached_tie_task_fault_free_5872, task_scope_swallow_keeps_cached_tie) {
    BEZEL_EXPECT_EQ(
        asanProbe(std::string("module M; import System.Threading; ") + kFailedType +
                  "class C "
                  "  public static async method Main(): Task<int> "
                  "    await using var scope := new TaskScope(); "
                  "    scope.Spawn(async lambda() => do _ := FS._v; end); "
                  "    scope.Spawn(async lambda() => do _ := FS._v; end); "
                  "    try await scope.Join(); catch var e: Exception => do end end; "
                  "    try _ := FS._v; "
                  "    catch var ex: TypeInitializationException => do return 55; end end; "
                  "    return 0; "
                  "  end Main; "
                  "end C;",
                  "bezel_5872_scope_swallow"),
        0);
}

BEZEL_TEST(cached_tie_task_fault_free_5872, task_scope_catch_arm_still_receives_tie) {
    auto r = runAot(std::string("module M; import System.Threading; ") + kFailedType +
                    "class C "
                    "  public static async method Main(): Task<int> "
                    "    await using var scope := new TaskScope(); "
                    "    scope.Spawn(async lambda() => do _ := FS._v; end); "
                    "    scope.Spawn(async lambda() => do _ := FS._v; end); "
                    "    try await scope.Join(); catch var e: Exception => do end end; "
                    "    try _ := FS._v; "
                    "    catch var ex: TypeInitializationException => do return 55; end end; "
                    "    return 0; "
                    "  end Main; "
                    "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 55);
}

// DIFFERENTIAL CONTROL — the direction that regresses if the fix were "stop
// freeing TIEs". 200 ordinary `throw new ArgumentException` / catch pairs,
// none of them a §22.7 cached instance, must still dispose. detect_leaks is ON
// here precisely so an un-disposed ordinary exception IS reported — a green row
// means the guard is narrow: it keys on the runtime-owned registry, not on
// "is a TypeInitializationException".
BEZEL_TEST(cached_tie_task_fault_free_5872, ordinary_throw_still_disposes) {
    BEZEL_EXPECT_EQ(
        asanProbeLeaky("module M; "
                  "class C "
                  "  public static method Main(): int "
                  "    var acc: int := 0; "
                  "    var i: int := 0; "
                  "    while i < 200 do "
                  "      try throw new ArgumentException(\"x\"); "
                  "      catch var e: ArgumentException => do acc := acc + 1; end end; "
                  "      i := i + 1; "
                  "    end; "
                  "    return acc; "
                  "  end Main; "
                  "end C;",
                  "bezel_5872_ordinary"),
        0);
}