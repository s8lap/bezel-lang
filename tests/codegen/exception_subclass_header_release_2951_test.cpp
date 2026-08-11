// Spec-derived regression test for issue #2951.
//
// §28.4 (The `destructor`, step 3) / §28.2 (exactly-once) / §28.9
// (Runtime-managed types) / §31.4 (Standard exception types) / §28.6 (repeat
// dispatch) / §31.2 (Catching), v0.240.
//
// THE DEFECT. Nothing released a user exception subclass's inherited `Message`
// (or `ArgumentException`'s `ParamName`). §28.4's generated `Dispose()` step 3
// states, verbatim: "Chains to the base class's `Dispose()`." `Emitter::
// emitBaseDisposeChain` iterates `TypeSymbol::baseTypes`, which holds only
// ancestors that HAVE a TypeSymbol — a built-in exception has none — so for
// `class SubEx: Exception` step 3 chained to NOTHING, and the shared
// `bz$exDisposeMsg` a directly-constructed `new Exception(msg)` disposes through
// was never reached from a subclass. Measured on the 20-iteration loop below:
//
//     ERROR: LeakSanitizer: detected memory leaks
//     SUMMARY: AddressSanitizer: 711 byte(s) leaked in 40 allocation(s).
//
// against the IDENTICAL loop on the built-in `Exception`, which is clean. The
// `(message, paramName)` overload leaked more (measured 1492 B / 84 allocations
// on the cross-module spelling), because the ParamName slot leaked too.
//
// WHY IT COULD NOT LAND BEFORE. Both blockers are in this same batch, and both
// were measured rather than reasoned about:
//
//   * #2902 — the `base(msg)` chain stored the message UN-RETAINED, so the slot
//     held a reference it did not own. Adding this release on that base freed
//     the caller's string: `heap-use-after-free ... #2 bz$exDisposeMsg`, where
//     the same program without the release is clean. That is the direction that
//     matters: a leak is not the worst outcome, an over-release is.
//   * #2950 — an IMPORTED subclass was laid out WITHOUT the §31.4 header, so the
//     exporter-compiled disposer handed the importer's FIELD VALUE to
//     `bz_string_release`: `SEGV on unknown address 0x2b`, rdi = 0x13 (the
//     field's value, 19), in `bz$TsLib2867T1.MyEx2867$DisposeCore` — i.e. the
//     existing `xmod_object_tostring_producer_residual_2867` test.
//
// ORACLES — BOTH DIRECTIONS, because §28.2 is exactly-once and LSan only sees
// one of them.
//
//   UNDER-release: every cell runs under `runAotChecked` (generated-code
//   ASan WITH LeakSanitizer), loop-amplified 20x with a churn allocation
//   between iterations, and every message/paramName is built at RUNTIME
//   (`"m" + i.ToString()`) — a literal is an immortal sentinel on which both
//   retain and release are no-ops, which is exactly how this defect survived
//   every literal-message example in the suite. A one-shot `Main` is not a
//   reliable LSan oracle either: a leftover pointer in a register makes a
//   leaked block read as merely reachable.
//
//   OVER-release: LSan is BLIND to it, so it is measured directly. T3/T4 keep
//   an INDEPENDENT reference to the message alive across the disposal and read
//   it AFTER, with churn in between so a freed block is reused rather than
//   merely stale. One `+1` string is aliased into TWO subclass instances; if
//   either disposal over-releases, the local's reference dies early and the
//   read faults under ASan. They also carry a LIVE-OBJECT COUNTER — a `Res`
//   class whose constructor increments and whose destructor decrements a static
//   — held as an `owned` field of the exception, asserted `= 0` at the end.
//   That is a two-sided count: a skipped cascade leaves it positive, a
//   double-disposed one leaves it negative, and only exactly-once leaves 0.
//
// "exit 0" is never the whole oracle: each `Main` recomputes a total from the
// values it read and returns a distinct non-zero code on a mismatch, so a
// degraded null or empty string fails loudly rather than passing quietly. Note
// the total ALONE would not have caught this defect — a leaked block still
// holds its bytes — so the sanitizer verdict is load-bearing and the totals
// guard the other direction.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

namespace {

using bezel::test::JitResult;
using bezel::test::runAotChecked;

// The oracle is the HARNESS verdict, not just the exit code: a sanitizer
// report (leak OR fault), a crash, or a run that never terminates all surface
// as ran=false and collapse to -999 here.
int run(const std::string& src) {
    JitResult r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// The live-object counter (the established idiom in this suite): constructor
// increments, destructor decrements, so the static is the number of LIVE
// instances. Held as an `owned` field of the exception subclass, so it counts
// the §28.4 step-2 owned-field cascade running exactly once per object —
// the cascade this fix emits new code immediately after.
const char* kRes =
    "class Res2951 public static live: int := 0; "
    "  public constructor() Res2951.live := Res2951.live + 1; end constructor; "
    "  destructor Res2951.live := Res2951.live - 1; end destructor; "
    "end Res2951; ";

}  // namespace

// ---------------------------------------------------------------------------
// T1 — the issue's own repro: a BORROWED message parameter. Post-#2902 the
// `base(m)` chain RETAINS a borrow, so the slot owns an independent `+1` that
// only this fix's release can return. §28.9 / §28.4 step 3.
//
// Fails without the fix, MEASURED: `711 byte(s) leaked in 40 allocation(s)`
// (20 message headers + their 20 data buffers), harness ran=false -> -999.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_exception_subclass_header_release_2951,
           borrow_message_released_exactly_once_spec_28_4_28_9) {
    // per §28.4 step 3 / §28.9 / §28.2
    BEZEL_EXPECT_EQ(run(
        "module M2951T1; "
        "class SubEx2951: Exception "
        "  public constructor(m: string) base(m); end constructor; "
        "end SubEx2951; "
        "class C2951T1 "
        "  public static method Main(): int "
        "    var t: int := 0; "
        "    var i: int := 0; "
        "    while i < 20 do "
        "      i := i + 1; "
        "      using var e: SubEx2951 := new SubEx2951(\"m\" + i.ToString()); "
        "      var churn: string := \"c\" + i.ToString(); "
        "      t := t + e.Message.Length + churn.Length; "
        "    end while; "
        // 9x(2+2) + 11x(3+3) = 36 + 66 = 102
        "    if t <> 102 then return 3; end if; "
        "    return 0; "
        "  end Main; "
        "end C2951T1;"), 0);
}

// ---------------------------------------------------------------------------
// T2 — the `+1` PRODUCER spelling: the message is built INSIDE the constructor
// from its own parameter, so `base(...)` TRANSFERS the single reference into
// the slot rather than retaining a borrow. The two spellings take different
// arms of #2902's retain/transfer decision and must both end exactly-once —
// which is why they are separate cells, not one.
//
// Fails without the fix, MEASURED: the same 711 B / 40 allocations.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_exception_subclass_header_release_2951,
           producer_message_released_exactly_once_spec_28_9_22_8) {
    // per §28.9 / §22.8 / §28.4 step 3
    BEZEL_EXPECT_EQ(run(
        "module M2951T2; "
        "class SubEx2951: Exception "
        "  public constructor(n: int) base(\"m\" + n.ToString()); end constructor; "
        "end SubEx2951; "
        "class C2951T2 "
        "  public static method Main(): int "
        "    var t: int := 0; "
        "    var i: int := 0; "
        "    while i < 20 do "
        "      i := i + 1; "
        "      using var e: SubEx2951 := new SubEx2951(i); "
        "      var churn: string := \"c\" + i.ToString(); "
        "      t := t + e.Message.Length + churn.Length; "
        "    end while; "
        "    if t <> 102 then return 3; end if; "
        "    return 0; "
        "  end Main; "
        "end C2951T2;"), 0);
}

// ---------------------------------------------------------------------------
// T3 — THE EXACTLY-ONCE CELL (§28.2), both directions in one program.
//
// OVER-release: one `+1` string, owned by the LOCAL `m`, is aliased into TWO
// subclass instances. Both are disposed at the inner block's end; `m` is read
// AFTER, with a churn allocation in between so a freed block gets reused rather
// than merely going stale. Two independent over-releases (one per instance)
// would drive the string's refcount below its true owner's and free it — the
// read then faults under ASan. This is the direction LSan cannot see, and it is
// the direction the #2902 blocker's damage lived in.
//
// LIVE-OBJECT COUNT: each instance owns a `Res2951`, so 40 are constructed and
// `Res2951.live` must be exactly 0 at the end — positive means a skipped
// cascade, negative means a double-disposed one. This fix emits its call
// immediately after that cascade, so the count is the direct evidence it did
// not disturb it.
//
// UNDER-release: `runAotChecked` keeps LSan on for the same program.
//
// Fails without the fix, MEASURED: the leak (LSan) — the over-release and the
// count pass either way here BY DESIGN, since the pre-fix code released
// nothing; they are what makes this a genuine exactly-once assertion rather
// than a leak assertion, and they are what would fail if a future change
// released twice.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_exception_subclass_header_release_2951,
           aliased_message_survives_both_disposals_spec_28_2) {
    // per §28.2 (exactly-once) / §28.4 step 2+3 / §28.9
    BEZEL_EXPECT_EQ(run(
        std::string("module M2951T3; ") + kRes +
        "class SubEx2951: Exception "
        "  private owned _r: Res2951; "
        "  public constructor(m: string) base(m); "
        "    _r := new Res2951(); "
        "  end constructor; "
        "end SubEx2951; "
        "class C2951T3 "
        "  public static method Main(): int "
        "    var t: int := 0; "
        "    var i: int := 0; "
        "    while i < 20 do "
        "      i := i + 1; "
        "      var m: string := \"shared\" + i.ToString(); "
        "      if true then "
        "        using var a: SubEx2951 := new SubEx2951(m); "
        "        using var b: SubEx2951 := new SubEx2951(m); "
        "        t := t + a.Message.Length + b.Message.Length; "
        "      end if; "
        // Both disposed above. If either over-released the aliased message,
        // this read is a use-after-free.
        "      var churn: string := \"c\" + i.ToString(); "
        "      t := t + m.Length + churn.Length; "
        "    end while; "
        "    if Res2951.live <> 0 then return 4; end if; "
        // 9x(3x7+2) + 11x(3x8+3) = 9x23 + 11x27 = 207 + 297 = 504
        "    if t <> 504 then return 3; end if; "
        "    return 0; "
        "  end Main; "
        "end C2951T3;"), 0);
}

// ---------------------------------------------------------------------------
// T4 — the MULTI-LEVEL hierarchy, which is where a naive version of this fix
// releases TWICE. The gate is the TRANSITIVE `hasBuiltinExceptionBase`, so both
// `ExA2951`'s and `ExB2951`'s cores carry the release, and `emitBaseDisposeChain`
// runs ExA's core from ExB's — two calls per object. It is exactly-once anyway
// because `bz$exDisposeMsg` NULLS each slot after releasing it and
// `bz_string_release` is null-safe, so the second is a no-op.
//
// Carries the same aliased-message over-release oracle as T3, because that
// double call is precisely the shape it would catch.
//
// Fails without the fix, MEASURED: the LSan leak.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_exception_subclass_header_release_2951,
           multi_level_hierarchy_releases_once_per_object_spec_28_4_12) {
    // per §28.4 step 3 / §12 (Inheritance) / §28.2
    BEZEL_EXPECT_EQ(run(
        std::string("module M2951T4; ") + kRes +
        "class ExA2951: Exception "
        "  private owned _r: Res2951; "
        "  public constructor(m: string) base(m); "
        "    _r := new Res2951(); "
        "  end constructor; "
        "end ExA2951; "
        "class ExB2951: ExA2951 "
        "  public B: int; "
        "  public constructor(m: string, n: int) base(m); "
        "    B := n; "
        "  end constructor; "
        "end ExB2951; "
        "class C2951T4 "
        "  public static method Main(): int "
        "    var t: int := 0; "
        "    var i: int := 0; "
        "    while i < 20 do "
        "      i := i + 1; "
        "      var m: string := \"shared\" + i.ToString(); "
        "      if true then "
        "        using var a: ExB2951 := new ExB2951(m, i); "
        "        using var b: ExB2951 := new ExB2951(m, i); "
        "        t := t + a.Message.Length + b.Message.Length + a.B; "
        "      end if; "
        "      var churn: string := \"c\" + i.ToString(); "
        "      t := t + m.Length + churn.Length; "
        "    end while; "
        "    if Res2951.live <> 0 then return 4; end if; "
        // T3's 504 plus sum(a.B) = sum(i) = 210.
        "    if t <> 714 then return 3; end if; "
        "    return 0; "
        "  end Main; "
        "end C2951T4;"), 0);
}

// ---------------------------------------------------------------------------
// T5 — §28.6 REPEAT DISPATCH: an explicit `Dispose()` followed by the scope-end
// disposal of the same object. The public wrapper's run-once flag stops the
// second entry, and `bz$exDisposeMsg`'s slot-nulling makes the release
// idempotent even if it did not — belt and braces, and this cell pins that a
// second dispatch does not re-release the message.
//
// Fails without the fix, MEASURED: the LSan leak.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_exception_subclass_header_release_2951,
           repeat_dispatch_does_not_re_release_spec_28_6) {
    // per §28.6 (repeat dispatch) / §28.2 / §28.4 step 3
    BEZEL_EXPECT_EQ(run(
        "module M2951T5; "
        "class SubEx2951: Exception "
        "  public constructor(m: string) base(m); end constructor; "
        "end SubEx2951; "
        "class C2951T5 "
        "  public static method Main(): int "
        "    var t: int := 0; "
        "    var i: int := 0; "
        "    while i < 20 do "
        "      i := i + 1; "
        "      using var e: SubEx2951 := new SubEx2951(\"m\" + i.ToString()); "
        "      var churn: string := \"c\" + i.ToString(); "
        "      t := t + e.Message.Length + churn.Length; "
        "      e.Dispose(); "
        "    end while; "
        "    if t <> 102 then return 3; end if; "
        "    return 0; "
        "  end Main; "
        "end C2951T5;"), 0);
}

// ---------------------------------------------------------------------------
// T6 — the ParamName slot (§31.4: `ArgumentException` declares `public readonly
// property ParamName: string?`). `bz$exDisposeMsg` releases Message AND
// ParamName; this cell keeps the second slot honest, since a fix that released
// only the message would still pass T1/T2.
//
// The paramName is read back through the built-in base's static type, because a
// subclass does not expose `ParamName` through its own (that is #2971, a
// separate gap), and through a §7.4 null-guard, because §31.4 declares it
// `string?` ("null when not supplied"). Its VALUE being right is what proves
// the slot the disposer releases is the slot the constructor wrote — and the
// guard being TAKEN every iteration is what stops the cell degrading into
// "ParamName was null, so nothing was measured".
//
// Fails without the fix, MEASURED: the leak, larger than T1's because both
// slots leak (1492 B / 84 allocations on the cross-module spelling of this).
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_exception_subclass_header_release_2951,
           paramname_slot_released_exactly_once_spec_31_4) {
    // per §31.4 (ArgumentException.ParamName) / §28.9 / §28.4 step 3
    BEZEL_EXPECT_EQ(run(
        "module M2951T6; "
        "class ArgEx2951: ArgumentException "
        "  public constructor(n: int) base(\"m\" + n.ToString(), \"p\" + n.ToString()); "
        "  end constructor; "
        "end ArgEx2951; "
        "class C2951T6 "
        "  public static method Main(): int "
        "    var t: int := 0; "
        "    var i: int := 0; "
        "    while i < 20 do "
        "      i := i + 1; "
        "      using var e: ArgEx2951 := new ArgEx2951(i); "
        "      var base2: ArgumentException := e; "
        "      var pn: string? := base2.ParamName; "
        // §7.4 narrowing. If ParamName were ever null the total falls short and
        // this cell fails, so the guard cannot silently skip the measurement.
        "      if pn <> null then "
        "        t := t + pn.Length; "
        "      end if; "
        "      var churn: string := \"c\" + i.ToString(); "
        "      t := t + e.Message.Length + churn.Length; "
        "    end while; "
        // 9x(2+2+2) + 11x(3+3+3) = 54 + 99 = 153
        "    if t <> 153 then return 3; end if; "
        "    return 0; "
        "  end Main; "
        "end C2951T6;"), 0);
}

// ---------------------------------------------------------------------------
// T7 — the THROWN-and-CAUGHT path (§31.2). An exception that is thrown reaches
// its disposal through the catch arm's discharge rather than a `using var`
// scope end, and that is a different site — a fix that only covered the
// scope-end path would pass every cell above and still leak every exception a
// program actually raises.
//
// Fails without the fix, MEASURED: the LSan leak.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_exception_subclass_header_release_2951,
           thrown_and_caught_subclass_releases_its_message_spec_31_2) {
    // per §31.2 (Catching) / §31.1 / §28.4 step 3 / §28.2
    BEZEL_EXPECT_EQ(run(
        "module M2951T7; "
        "class SubEx2951: Exception "
        "  public constructor(m: string) base(m); end constructor; "
        "end SubEx2951; "
        "class C2951T7 "
        "  public static method Raise(n: int): int "
        "    throw new SubEx2951(\"m\" + n.ToString()); "
        "  end Raise; "
        "  public static method Main(): int "
        "    var t: int := 0; "
        "    var i: int := 0; "
        "    while i < 20 do "
        "      i := i + 1; "
        "      try "
        "        var d := Raise(i); "
        "        return 5; "
        "      catch var e: SubEx2951 => t := t + e.Message.Length; "
        "      end; "
        "      var churn: string := \"c\" + i.ToString(); "
        "      t := t + churn.Length; "
        "    end while; "
        "    if t <> 102 then return 3; end if; "
        "    return 0; "
        "  end Main; "
        "end C2951T7;"), 0);
}
