// Fix #4361: Lock.Dispose() called twice on the same variable double-frees
// the bz_lock_t struct.
//
// Bug: bz_lock_release frees the bz_lock_t struct and releases the mutex
// core on EVERY call (lines 137-139 of bezel_runtime_sync.cpp), not just the
// first.  The "idempotent" claim in the comment (line 125-127) is wrong: the
// released-flag guard (lines 131-134) only prevents double pthread_mutex_unlock,
// but delete lk and mutex_core_release(m) run unconditionally on every call.
//
// Calling Dispose() twice on the same Lock variable:
//   guard.Dispose();  // bz_lock_release: unlocks, deletes lk, releases core
//   guard.Dispose();  // bz_lock_release: reads freed lk->mutex, double-deletes
//
// ASan must detect the double-free / UAF.
//
// stdlib §6.2 (v0.138, #647) / #4361.

#include "support/aot_support.hpp"   // runAot
#include "support/codegen_support.hpp"
#include "support/run_under_asan.hpp"  // runUnderAsan
#include "test_harness.hpp"
#include "native_test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// ASan probe: 0 = clean; 1 = ASan/LSan fired; -1 = infra unavailable (SKIP).
int runUnderAsan(const std::string& source) {
    return bezel::test::runUnderAsan(source, "bezel_4361_asan");
}

}  // namespace

// stdlib §6.2 / #4361: explicit double-Dispose on a Lock — ASan catches the
// double-free of the bz_lock_t struct.
BEZEL_TEST(lock_double_dispose_4361, double_dispose_double_free_asan) {
    int r = runUnderAsan(
        "module M; import System.Threading; "
        "class C "
        "  public static method Main(): int "
        "    using var mx := new Mutex(); "
        "    using var guard := mx.Acquire(); "
        "    guard.Dispose(); "
        "    guard.Dispose(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (r != -1) BEZEL_EXPECT_EQ(r, 0);
}

// stdlib §6.2: single Dispose of a Lock is valid — no error expected.
BEZEL_TEST(lock_double_dispose_4361, single_dispose_releases_lock) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; "
        "class C "
        "  public static method Main(): int "
        "    using var mx := new Mutex(); "
        "    using var guard := mx.Acquire(); "
        "    guard.Dispose(); "
        "    using var guard2 := mx.Acquire(); "
        "    guard2.Dispose(); "
        "    return 0; "
        "  end Main; "
        "end C;"), 0);
}

// stdlib §6.2: TryAcquire succeeds on uncontested mutex.
BEZEL_TEST(lock_double_dispose_4361, try_acquire_succeeds) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; "
        "class C "
        "  public static method Main(): int "
        "    using var mx := new Mutex(); "
        "    using var guard := mx.TryAcquire(TimeSpan.FromSeconds(1)); "
        "    if guard <> null then "
        "      return 7; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 7);
}

// stdlib §6.2: TryAcquire timeout on held mutex returns null.
BEZEL_TEST(lock_double_dispose_4361, try_acquire_timeout_on_held) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; "
        "class C "
        "  public static method Main(): int "
        "    using var mx := new Mutex(); "
        "    using var outer := mx.Acquire(); "
        "    using var guard := mx.TryAcquire(TimeSpan.FromMilliseconds(0)); "
        "    if guard = null then "
        "      return 99; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 99);
}
