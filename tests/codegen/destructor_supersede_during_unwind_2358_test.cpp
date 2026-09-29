// Codegen — issue #2358 / spec §28.4 + §31.2 (finally-supersession
// discipline), docs revision v0.233.
//
// §28.4 "The `destructor`" (v0.233), the throwing-destructor paragraph:
//   "In a multi-value cleanup pass (scope-end disposal of several locals, an
//   owning collection's element cascade, the exception-path discharge —
//   including the unwind disposal of an expression's produced temporaries,
//   §28.3), a throwing disposer does not skip the remaining values: every
//   remaining disposal still runs. If a later disposal in the same pass also
//   throws, the later exception supersedes the earlier one — the §31.2
//   `finally`-supersession discipline — and the superseded exception is
//   disposed (the two owner-owned instances excepted, §31.2)."
//
// §31.2 "`try`/`catch`/`finally`" (v0.233), the supersession rule this cites:
//   "A throw escaping a `finally` supersedes the in-flight exception ...
//   when a `finally` entered during exception propagation itself completes
//   by throwing (directly, or via a call that propagates), the new
//   exception replaces the one being handled; the superseded exception is
//   disposed by the runtime exactly once, as if its handling had completed
//   — unless it is one of the two §31.1 dispose-after-arm exemptions: a
//   superseded task-owned fault instance (§30.9) or cached
//   TypeInitializationException (§31.4) is dropped from the in-flight slot
//   undisposed ... The superseding exception then propagates from the `try`
//   statement as usual."
//
// VERIFY-FIRST finding (synchronous-only repro, no async/iterators): when a
// `using var`'s destructor throws while ANOTHER exception is already
// unwinding the frame past it (the §28.3 "exception-path discharge"), the
// destructor's new exception correctly SUPERSEDES and reaches the matching
// catch arm (that routing was already correct) — but the superseded original
// exception is never disposed: it is silently dropped from the in-flight
// slot, leaking 48 bytes (its `calloc`'d object) under LeakSanitizer. This
// is the SAME rule §28.4/§31.2 already state for a `finally`/catch-guard
// supersede (LlvmBackend_Stmts.cpp's #1149/#1108/#1216 fixes) — it was just
// never wired into the SEPARATE code path that runs when the exception is
// superseded from INSIDE a destructor body during an already-active unwind
// (Emitter::emitDisposerCore's post-body restore in LlvmBackend_Disposal.cpp,
// which silently dropped `parkedDtorExc`, the exception captured at disposer
// entry, whenever the body itself threw a genuinely new one).
//
// FIX: emitDisposerCore now disposes+reclaims the parked (superseded)
// exception exactly once — mirroring emitPendingCheckIf / emitNewPendingCheck
// (#1216/#1149)'s existing supersede-and-free pattern, including the
// §30.9 deferred-fault-record clear — gated on BOTH "the destructor itself
// threw a new exception" AND "an exception was genuinely in flight at
// this disposal's entry" (a runtime null check on the parked value), so the
// ordinary non-unwind throwing-destructor path (no exception in flight; a
// SEPARATE, already-tracked gap — see below) takes an unchanged path.
//
// SCOPE NOTE — control case NOT pinned here: a destructor that throws on a
// NORMAL-path scope exit (no unwind already in progress) was, when #2358
// landed, silently swallowed rather than reaching the enclosing catch — a
// distinct defect (issues #1878, #1882) that #2358 did not touch. Verified via
// scratch-copy revert/restore that this control case's behavior was IDENTICAL
// before and after this fix; deliberately not pinned here, to avoid enshrining
// that swallowing as if it were #2358's contract. It has since been FIXED (the
// missing §28.4 propagate-from-the-disposal-point check, #1878/#1882) and is
// pinned in its own files —
// throwing_destructor_propagates_from_{explicit_dispose_1878,scope_end_1882}_test.cpp.
// The rows below are unaffected: every one of them faults on the UNWIND edge,
// which #1878/#1882 deliberately left untouched.
//
// FAILS WITHOUT / PASSES WITH: every runAotChecked test below whose comment
// says "48-byte leak without the fix" flips r.ran from false to true
// (confirmed via scratch-copy git-checkout revert + rebuild + re-run, NOT
// git stash — see the issue's verification notes).

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §28.4/§31.2 (the core #2358 repro, lightly adapted from the issue's own
// minimal synchronous reproduction): `using var r`'s destructor throws
// DisposalFault while Boom is already unwinding past it. DisposalFault must
// supersede and reach its own catch arm (return 20) — that routing was
// already correct pre-fix — AND Boom must be disposed exactly once: without
// the fix this leaks 48 bytes (Boom's object) under LeakSanitizer.
BEZEL_TEST(destructor_supersede_during_unwind_2358,
           original_exception_disposed_when_superseded_during_active_unwind) {
    auto r = runAotChecked(
        "module M; "
        "class ResThrows "
        "  destructor throw new DisposalFault(); end destructor; "
        "end ResThrows; "
        "class Boom: Exception "
        "  public constructor() base(\"boom\"); end constructor; "
        "end Boom; "
        "class DisposalFault: Exception "
        "  public constructor() base(\"disposal-fault\"); end constructor; "
        "end DisposalFault; "
        "class C "
        "  public static method Main(): int "
        "    try "
        "      using var r := new ResThrows(); "
        "      throw new Boom(); "
        "      return 1; "
        "    catch var ex: Boom => do return 10; end, "
        "      var ex: DisposalFault => do return 20; end "
        "    end; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);   // false pre-fix: 48-byte leak of Boom
    BEZEL_EXPECT_EQ(r.exitCode, 20);
}

// §28.4's "a throwing disposer does not skip the remaining values" combined
// with the supersede rule, CHAINED: two owned locals, BOTH with throwing
// destructors, unwinding because of an original Boom. Reverse-declaration-
// order disposal means `a` (declared second) is disposed first — superseding
// Boom with AFault, disposing Boom — then `b` (declared first) is disposed —
// superseding AFault with BFault, disposing AFault. BFault (the final
// survivor) reaches its catch arm; Boom AND AFault must both be disposed
// exactly once. Without the fix this leaks 96 bytes (Boom + AFault).
BEZEL_TEST(destructor_supersede_during_unwind_2358,
           chained_supersession_disposes_every_intermediate_exception) {
    auto r = runAotChecked(
        "module M; "
        "class AThrows "
        "  destructor throw new AFault(); end destructor; "
        "end AThrows; "
        "class BThrows "
        "  destructor throw new BFault(); end destructor; "
        "end BThrows; "
        "class Boom: Exception "
        "  public constructor() base(\"boom\"); end constructor; "
        "end Boom; "
        "class AFault: Exception "
        "  public constructor() base(\"a-fault\"); end constructor; "
        "end AFault; "
        "class BFault: Exception "
        "  public constructor() base(\"b-fault\"); end constructor; "
        "end BFault; "
        "class C "
        "  public static method Main(): int "
        "    try "
        "      using var b := new BThrows(); "
        "      using var a := new AThrows(); "
        "      throw new Boom(); "
        "      return 1; "
        "    catch var ex: Boom => do return 10; end, "
        "      var ex: AFault => do return 11; end, "
        "      var ex: BFault => do return 12; end "
        "    end; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);   // false pre-fix: 96-byte leak (Boom + AFault)
    BEZEL_EXPECT_EQ(r.exitCode, 12);
}

// §28.4/§31.4: the ORIGINAL in-flight exception being superseded is itself a
// TypeInitializationException (§22.13 static-init wrap around FS's failing
// field initializer). Confirms the superseded TIE wrapper is disposed
// correctly (its own Message/ParamName/CauseChain — all null for a TIE — and
// its 40-byte block) WITHOUT touching InnerException@16 — the still-live
// original ArgumentException stays valid for a LATER touch of the failed
// type, per §31.4's "the cached TypeInitializationException and its
// InnerException are runtime-owned for the life of the process" language
// (an established, already-shared invariant of the built-in TIE disposer —
// `bz$exDisposeMsg` deliberately skips InnerException — not something this
// fix has to special-case). Without the fix this leaks 40 bytes (the TIE
// wrapper).
BEZEL_TEST(destructor_supersede_during_unwind_2358,
           superseding_a_type_initialization_exception_disposes_the_wrapper) {
    auto r = runAotChecked(
        "module M; "
        "class FS "
        "  public static _v: int := Throw(); "
        "  private static method Throw(): int "
        "    throw new ArgumentException(\"inner-msg\"); return 0; "
        "  end Throw; "
        "end FS; "
        "class ResThrows "
        "  destructor throw new DisposalFault(); end destructor; "
        "end ResThrows; "
        "class DisposalFault: Exception "
        "  public constructor() base(\"disposal-fault\"); end constructor; "
        "end DisposalFault; "
        "class C "
        "  public static method Main(): int "
        "    var first: int := 0; "
        "    try "
        "      using var r := new ResThrows(); "
        "      _ := FS._v; "
        "      return 1; "
        "    catch var ex: TypeInitializationException => do first := 20; end, "
        "      var ex: DisposalFault => do first := 30; end "
        "    end; "
        // A second touch after the fault must still reach
        // TypeInitializationException with a live InnerException — proving
        // the cached inner survived the first TIE's supersede-dispose above.
        "    try "
        "      _ := FS._v; "
        "      return 1; "
        "    catch var ex: TypeInitializationException => do "
        // Native process exit codes are 8-bit (WEXITSTATUS), so encode the
        // two facts being checked — first==30 (DisposalFault won the first
        // supersede) AND the second touch's InnerException is still live —
        // as a single small code rather than a value that would silently
        // truncate mod 256.
        "      if first = 30 and ex.InnerException is not null then "
        "        return 77; "
        "      else "
        "        return first; "
        "      end; "
        "    end "
        "    end; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);   // false pre-fix: 40-byte leak of the TIE wrapper
    BEZEL_EXPECT_EQ(r.exitCode, 77);  // first=30 (DisposalFault won) AND inner alive
}

// Adversarial self-pass: a `using var` declared INSIDE an explicit `finally`
// block (not a plain try-body local) whose destructor throws while the
// finally is handling an already-in-flight Boom. This exercises BOTH the
// pre-existing currentCaughtExc_ finally-supersede mechanism (LlvmBackend_
// Stmts.cpp's #1149/#1108) AND this fix's disposerEntryExc_/parkedDtorExc
// mechanism at what looks like the same call site — confirms they do NOT
// double-dispose Boom (this scenario already passed before the fix too;
// scratch-copy revert confirmed identical before/after — pinned here as
// permanent interaction coverage for the code this fix touches).
BEZEL_TEST(destructor_supersede_during_unwind_2358,
           using_var_inside_finally_does_not_double_dispose_the_original) {
    auto r = runAotChecked(
        "module M; "
        "class ResThrows "
        "  destructor throw new DisposalFault(); end destructor; "
        "end ResThrows; "
        "class Boom: Exception "
        "  public constructor() base(\"boom\"); end constructor; "
        "end Boom; "
        "class DisposalFault: Exception "
        "  public constructor() base(\"disposal-fault\"); end constructor; "
        "end DisposalFault; "
        "class C "
        "  public static method Main(): int "
        "    try "
        "      try "
        "        throw new Boom(); "
        "        return 1; "
        "      finally "
        "        using var r := new ResThrows(); "
        "      end; "
        "    catch var ex: Boom => do return 10; end, "
        "      var ex: DisposalFault => do return 20; end "
        "    end; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);   // no leak/UAF/double-free either way
    BEZEL_EXPECT_EQ(r.exitCode, 20);
}

// Control: the OVERWHELMINGLY common case — a WELL-BEHAVED (non-throwing)
// destructor disposed during an active unwind. `parkedDtorExc` is non-null
// (Boom is in flight) but the destructor never throws, so the fix's
// supersede branch must NOT fire: Boom must reach its catch unchanged, and
// R's own (non-exceptional) cleanup must run exactly once — proving the fix
// does not perturb the ordinary single-exception path.
BEZEL_TEST(destructor_supersede_during_unwind_2358,
           non_throwing_destructor_during_unwind_is_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class R "
        "  public static disposed: int := 0; "
        "  destructor R.disposed := R.disposed + 1; end destructor; "
        "end R; "
        "class Boom: Exception "
        "  public constructor() base(\"boom\"); end constructor; "
        "end Boom; "
        "class C "
        "  public static method Main(): int "
        "    try "
        "      using var r := new R(); "
        "      throw new Boom(); "
        "      return 1; "
        "    catch var ex: Boom => do return 100 * R.disposed + 7; end "
        "    end; "
        "  end Main; "
        "end C;"),
        107);
}
