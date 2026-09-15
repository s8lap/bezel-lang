// Fix #4290 (reframed): owned `Mutex`/`Lock` class fields leak at disposal.
//
// Bug: `emitOwnedFieldDisposals` (LlvmBackend_Disposal.cpp) has no predicate
// matching a builtin vtableless type (`Mutex`, `Lock`, `Semaphore`, etc.) as
// an owned class field.  All the specific predicates —
// `isOwnedReferenceField`, `ownedCollectionFieldType`, `isOwnedSubscriptionField`,
// `isOwnedIteratorInterfaceField`, `isOwnedEventSourceField`,
// `isOwnedSharedWeakField`, `isOwnedFfiBufferField` — return false for these
// types (they have `declaration == nullptr` in their TypeRef and are not
// caught by the name-based arms).  The field falls through to the iteration
// loop silently skipped, and the `bz_mutex_t` (or `bz_lock_t`) allocation
// leaks at every class-disposal.
//
// The local `using var mx := new Mutex()` path works correctly:
// `registerUsingDisposal` has explicit Mutex/Lock arms that emit
// `bz_mutex_dispose` / `bz_lock_release`.  The class-field path needs the
// same arms added to `emitOwnedFieldDisposals`.
//
// Lock field disposal: the `using var guard := _mx.Acquire()` call acquires
// a Lock (returned owned, disposed at scope end), and the guard.Dispose()
// at scope end calls bz_lock_release correctly.  The gap is ONLY the
// _mx field itself (the Mutex object, not any Lock guard).
//
// stdlib §6.2 (v0.138, #647).

#include "support/aot_support.hpp"   // runAot / runAotCheckedDisposal
#include "support/codegen_support.hpp"
#include "support/run_under_asan.hpp"  // runUnderAsan
#include "test_harness.hpp"
#include "native_test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotCheckedDisposal;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// ASan probe: 0 = clean; 1 = ASan/LSan fired; -1 = infra unavailable (SKIP).
int runUnderAsan(const std::string& source, const char* stem) {
    return bezel::test::runUnderAsan(source, stem);
}

}  // namespace

// stdlib §6.2: owned Mutex field — the class instance must be disposed
// cleanly with no leak.
BEZEL_TEST(codegen_mutex_field_disposal, owned_mutex_field_no_leak) {
    int r = runUnderAsan(
        "module M; import System.Threading; "
        "class WithMutex "
        "  private owned readonly _mx: Mutex := new Mutex(); "
        "  public method Do(): int "
        "    using var guard := _mx.Acquire(); "
        "    return 42; "
        "  end Do; "
        "end WithMutex; "
        "class C "
        "  public static method Main(): int "
        "    using var wl := new WithMutex(); "
        "    return wl.Do(); "
        "  end Main; "
        "end C;",
        "bezel_4290_mutex_field");
    if (r == -1) return;  // toolchain unavailable: SKIP
    BEZEL_EXPECT_EQ(r, 0);  // 0 = clean (no leak)
}

// stdlib §6.2: functional correctness — owned Mutex field, acquire + release.
BEZEL_TEST(codegen_mutex_field_disposal, owned_mutex_field_acquire_release) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; "
        "class WithMutex "
        "  private owned readonly _mx: Mutex := new Mutex(); "
        "  public method Do(): int "
        "    using var guard := _mx.Acquire(); "
        "    return 42; "
        "  end Do; "
        "end WithMutex; "
        "class C "
        "  public static method Main(): int "
        "    using var wl := new WithMutex(); "
        "    return wl.Do(); "
        "  end Main; "
        "end C;"), 42);
}

// stdlib §6.2: owned Mutex field with explicit Dispose call on the instance.
BEZEL_TEST(codegen_mutex_field_disposal, owned_mutex_field_explicit_dispose) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; "
        "class WithMutex "
        "  private owned readonly _mx: Mutex := new Mutex(); "
        "  public method Do(): int "
        "    using var guard := _mx.Acquire(); "
        "    return 99; "
        "  end Do; "
        "end WithMutex; "
        "class C "
        "  public static method Main(): int "
        "    var wl := new WithMutex(); "
        "    var r := wl.Do(); "
        "    wl.Dispose(); "
        "    return r; "
        "  end Main; "
        "end C;"), 99);
}
