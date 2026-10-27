// Codegen — a bare `owned` BUILTIN handle (Shared<T> / Channel<T> / ChannelSender<T> /
// Weak<T>) transferred into an `owned` argument is disposed EXACTLY ONCE on the CAUGHT
// arg-throw path inside the SAME scope (issue #2000 / spec §28.4 / §28.6 / §28.13).
//
// ROOT CAUSE: the #1990 / #1992 fix (b3c3676) registered a transient unwind-only
// disposal for class/interface instances in `buildOrderedArgs` (LlvmBackend_Assign.cpp)
// on the local's OWN slot, in the CURRENT scope. The transient fires on BOTH the
// caught-throw edge and the propagate-throw edge (both go through the in-method
// scope's emitDisposals with isUnwind=true). That fix deliberately EXCLUDED builtin
// owned handles (Channel/ChannelSender/Shared/Weak) — the comment claimed they
// "are left to the declaring-scope registration", but for owned LOCAL handles of
// these types, `unwindOwnedLocalClass` returns null (no user TypeSymbol) so NO
// declaring-scope registration is created. The #1957 registerUnwindOwnedParam
// path covered owned PARAMETERS (registered at `disposeStack_.front()`, the
// method-body scope) but did not cover the LOCAL case. Result: a
//   try {
//     Sink(owned s, throws);
//   } catch e { ... }
// where `s` is an `owned Shared<X>` declared in the try-block scope leaks the
// Shared control block (and, by transitivity, the inner `X`) on the caught edge
// — the throw unwinds only to the in-method handler, neither the transient nor a
// declaring-scope disposal fires. (For an OWNED PARAM transferred to another
// call in the same try-block the leak is the same shape: registerUnwindOwnedParam
// registers at the OUTERMOST scope, so the caught edge also leaves it inactive;
// on the propagate edge it would be double-released by both the transient AND
// the outer registration without a slot-null after the transient's release.)
//
// FIX (src/codegen/LlvmBackend_Assign.cpp, buildOrderedArgs, ~line 4207;
// src/codegen/LlvmBackend_Disposal.cpp, ~line 1964): extend the same transient
// disposal to builtin ChannelSender<T> / Channel<T> / Shared<T> / Weak<T>
// handles detected by head name (mirroring the using-var path and #1957's
// registerUnwindOwnedParam branch); reuse the `unwindOnly` Disposal flag and
// dispatch the type-specific release (bz_channel_sender_release /
// bz_channel_dispose / bz_shared_release / bz_weak_release) before the generic
// vtable path in emitDisposals' `unwindOnly` block, then NULL THE SLOT so the
// declaring-scope disposal (param case) or any sibling rewrite (local case)
// null-guardedly skips — no double-release, dispose-exactly-once. Restricted to
// the compiler-known builtin head names — exactly the same set registerUnwindOwnedParam
// recognises. Other transient shape is unchanged.
//
// WITNESS (stronger than LSan — LSan conservative-stack-scans and can FALSE-CLEAN
// leaked owned locals): the destructor of the underlying value bumps a static
// counter, and Main READS that counter AFTER the catch returns to outer scope.
// On the buggy base, the Shared handle is never released on the caught edge →
// the inner `X`'s destructor never runs (the last disposal of the only strong
// handle triggers it) → counter == 0, the program returns the witness-fail
// value. With the fix, the transient fires on the catch edge, releases the
// Shared handle, refcount drops to 0, X's destructor runs once → counter == 1.
// LSan (runAotChecked, generated-code ASan/LSan) additionally fails the buggy
// build with r.ran=false (control-block leak). The ctor/dtor shape is
// dispose-exactly-once — a double-release would be an ASan hit and is also
// gated by r.ran.
//
// Applies uniformly to constructor and method-call arg lists (shared
// buildOrderedArgs path). The buildOrderedArgs transient for `new T(...)`
// transfers and for `f(...)` call argument lists is the same site.

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

#include <string>

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

// T1 — exact repro for the owned LOCAL case (#2000): a `var s := new Shared<X>(...)`
// declared in the try-block scope is transferred via `owned s` to a Sink; a LATER
// argument throws; the throw is CAUGHT in the SAME scope. The transient disposal
// added by #2000 must release the Shared handle on the caught edge — the inner X's
// destructor then runs EXACTLY once. Without the fix: the Shared handle is never
// released → X's destructor never runs → X._n == 0 → return 3; and LSan flags the
// control-block + 16-byte X body as leaked.
const char* kSharedLocalCaughtMethodCall =
    "module M; "
    "class X "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor X._n := X._n + 1; end destructor; "
    "end X; "
    "class P "
    "  public static method Sink(owned h: Shared<X>, n: int): int "
    "    h.Dispose(); return n; "                      // sole release on the normal path
    "  end Sink; "
    "  public static method Boom(): int throw new Exception(\"boom\"); end Boom; "
    "  public static method Main(): int "
    "    var caught := 0; "
    "    var s := new Shared<X>(new X()); "            // one strong handle, refcount=1
    "    try "
    "      P.Sink(move s, P.Boom()); "                // transfer s; Boom() throws
    "    catch Exception => caught := 1; end; "
    "    if caught <> 1 then return 2; end; "
    "    if X._n <> 1 then return 3; end; "            // shared released once on caught edge
    "    return 0; "
    "  end Main; "
    "end P;";

// T2 — same shape via the constructor arg list (`new Wrap(owned s, <throws>)`),
// caught in-method. Exercises the buildOrderedArgs transient path on the
// `new T(...)` arm — same transient site, different prologue.
const char* kSharedLocalCaughtCtor =
    "module M; "
    "class X "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor X._n := X._n + 1; end destructor; "
    "end X; "
    "class Wrap "
    "  public owned _h: Shared<X>; "
    "  public _b: int; "
    "  public constructor(owned h: Shared<X>, b: int) "
    "    _h := h; _b := b; "
    "  end constructor; "
    "  destructor end destructor; "                   // _h auto-released on dispose
    "end Wrap; "
    "class P "
    "  public static method Boom(): int throw new Exception(\"boom\"); end Boom; "
    "  public static method Main(): int "
    "    var caught := 0; "
    "    var s := new Shared<X>(new X()); "
    "    try "
    "      using var w := new Wrap(move s, P.Boom()); "
    "    catch Exception => caught := 1; end; "
    "    if caught <> 1 then return 2; end; "
    "    if X._n <> 1 then return 3; end; "            // shared released once on caught edge
    "    return 0; "
    "  end Main; "
    "end P;";

// T3 — T1, but TWO transferred owned Shared handles, same scope, caught. Each is
// released EXACTLY once on the caught edge. Mirrors #1992 ("all-but-the-last
// leak" parallel). Without fix: only the LATEST occasionally happens to be
// reclaimed → X._n in {0, 1}; return 3. With fix: both transient disposals fire
// → X._n == 2.
const char* kTwoSharedLocalCaught =
    "module M; "
    "class X "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor X._n := X._n + 1; end destructor; "
    "end X; "
    "class P "
    "  public static method Sink(owned a: Shared<X>, owned b: Shared<X>, n: int): int "
    "    a.Dispose(); b.Dispose(); return n; "
    "  end Sink; "
    "  public static method Boom(): int throw new Exception(\"boom\"); end Boom; "
    "  public static method Main(): int "
    "    var caught := 0; "
    "    var s := new Shared<X>(new X()); "
    "    var t := new Shared<X>(new X()); "
    "    try "
    "      P.Sink(move s, move t, P.Boom()); "       // both transferred; Boom() throws
    "    catch Exception => caught := 1; end; "
    "    if caught <> 1 then return 2; end; "
    "    if X._n <> 2 then return 3; end; "            // BOTH released once
    "    return 0; "
    "  end Main; "
    "end P;";

// T4 — the SUCCESSFUL path (no throw): transient must be INERT, the callee
// becomes the sole owner of the Shared handle, and the inner X is disposed when
// the Shared handle is dropped. Sink disposes its owned params on the normal
// path; X._n == 2 after both Shared handles are released.
//
// Companion to #1990's "successful construction" tests (#993 holding the
// transferred-TEMP precedent). The transient is `unwindOnly`, so it does NOT
// fire on a normal exit; the slot is nulled below the call at the #1918
// dispose-time so a sibling branch that did NOT transfer still finds null.
// Here both transfers complete, so the slot is nulled by the post-call transfer
// machinery (no leak — null-out + dispose-share on the Sink side is exactly
// once). LSan also verifies the run is leak-clean.
const char* kTwoSharedLocalSuccessful =
    "module M; "
    "class X "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor X._n := X._n + 1; end destructor; "
    "end X; "
    "class P "
    "  public static method Sink(owned a: Shared<X>, owned b: Shared<X>): int "
    "    a.Dispose(); b.Dispose(); return 0; "
    "  end Sink; "
    "  public static method Ident(v: int): int return v; end Ident; "
    "  public static method Main(): int "
    "    var s := new Shared<X>(new X()); "
    "    var t := new Shared<X>(new X()); "
    "    do "
    "      P.Sink(move s, move t); "                 // both transferred, no throw
    "    end; "                                        // s + t released in Sink
    "    if X._n <> 2 then return 5; end; "
    "    return 0; "
    "  end Main; "
    "end P;";

}  // namespace

// ── T1: #2000 — single owned Shared<T> local, caught in-method: handle released ─
BEZEL_TEST(builtin_owned_handle_try_catch_unwind_2000,
           shared_local_caught_method_call_released_once_spec_28_4_28_6_28_13) {
    JitResult r = runAotChecked(kSharedLocalCaughtMethodCall);
    BEZEL_EXPECT_TRUE(r.ran);                          // LSan-clean: s released, no leak
    BEZEL_EXPECT_EQ(r.exitCode, 0);                    // caught AND X._n == 1 (witness)
}

// ── T2: #2000 — single owned Shared<T> local, ctor arg list, caught: released ───
BEZEL_TEST(builtin_owned_handle_try_catch_unwind_2000,
           shared_local_caught_ctor_arg_released_once_spec_28_4_28_6_28_13) {
    JitResult r = runAotChecked(kSharedLocalCaughtCtor);
    BEZEL_EXPECT_TRUE(r.ran);                          // LSan-clean: ctor arg path
    BEZEL_EXPECT_EQ(r.exitCode, 0);                    // caught AND X._n == 1
}

// ── T3: #2000 sibling — two owned Shared<T> locals, caught: BOTH released once ─
BEZEL_TEST(builtin_owned_handle_try_catch_unwind_2000,
           two_shared_locals_caught_both_released_once_spec_28_4_28_6_28_13) {
    JitResult r = runAotChecked(kTwoSharedLocalCaught);
    BEZEL_EXPECT_TRUE(r.ran);                          // LSan-clean: both released, no leak
    BEZEL_EXPECT_EQ(r.exitCode, 0);                    // caught AND X._n == 2
}

// ── T4: #2000 — successful transfer (no throw): transient inert, no double-rel ─
BEZEL_TEST(builtin_owned_handle_try_catch_unwind_2000,
           shared_local_successful_transfer_no_double_release_spec_28_4_28_6_28_13) {
    JitResult r = runAotChecked(kTwoSharedLocalSuccessful);
    BEZEL_EXPECT_TRUE(r.ran);                          // LSan-clean: disposed once via Sink
    BEZEL_EXPECT_EQ(r.exitCode, 0);                    // X._n == 2 after both released
}
