// Codegen — #6382 / §28.9 + §29.11 (v0.338).
//
// THE ISSUE. `for var x in MakeA() do … end` where `MakeA(): IEnumerable<int>`
// returns `new SelfA(3)` and `SelfA` implements the enumerator interface ITSELF,
// with a self-returning `GetEnumerator()`. Filed as a "double free"; the
// byte-level reality is a **heap-use-after-free READ** — one `free`, one read of
// the 24-byte object's disposed flag.
//
// NOT THE #6378 MIRROR, and two of the issue's stated premises are false. All
// MEASURED by the diagnosing lane on the pre-fix tree; the discriminating
// measurements are repeated in the rows below so this file is not leaning on a
// quoted log:
//
//   * `enumIsTsOrBase` / `lowerIEnumerableFor` are NEVER REACHED. The source's
//     static type is the declaration-less builtin `IEnumerable<T>` sentinel, so
//     the for-in dispatcher tries `lowerIEnumerableBuiltinFor` first
//     (src/codegen/LlvmBackend_Stmts.cpp:4615) and `lowerIEnumerableFor`
//     (`:4617`) bails at `:10077`. Control row `class_typed_source_…` below is
//     the discriminator: it is the shape where `lowerIEnumerableFor` DOES run
//     and `enumIsTsOrBase` IS true, and it is ASan-clean on both sides.
//   * A SECOND IMPLEMENTER OF THE ENUMERATOR INTERFACE IS NOT REQUIRED.
//     Control row `single_implementer_…` below has no second implementer and
//     reproduces identically.
//   * THE `enumPtr == srcPtr` ALIAS IS NOT REQUIRED.
//     Control row `fresh_getenumerator_…` below returns a FRESH instance of the
//     class's own type and still reproduces.
//
// THE DEFECT. `wrapUserClassAsIEnumerable`'s cached `wrapDisposeFn`
// (src/codegen/LlvmBackend_Assign.cpp:14090) builds ONE disposal function per
// `(srcTs, enumTs)` PAIR and shares it across every wrapper instance of that
// pair — including the re-iteration CLONES minted by `wrapGetEnumFn`
// (`:14223-14257`). Two instances are built over the same source object P:
//
//   * W₀ — the ORIGINAL, minted by `wrapAsIEnumerableForTarget` on `MakeA`'s
//     return edge. `srcExpr` is `new SelfA(3)`, which has no `ctx_.exprSymbol`,
//     so `ownsSrc = !srcIsNamedOwner = true`. Slots `{vtable, P, P, true}`.
//   * W₁ — the RE-ITERATION CLONE, minted at for-in time by vtable slot 4. It
//     re-runs `srcGetEnumFn(srcObj)` and stores **`false`** into slot 3
//     (`:14249-14255`, #2424: "the clone NEVER owns srcObj"). Slots
//     `{vtable, P, P, false}`.
//
// The dispatch block made a per-INSTANCE disposal on a per-PAIR-constant
// decision: when `srcTs == enumTs` (`:14088`, #5203) it called
// `srcDisposeFn(srcObj)` **unconditionally**, without consulting the
// per-instance `ownsSrc` flag — the flag the SAME function already loads ten
// lines below to decide whether to reclaim. So W₁ disposed an object owned by
// W₀.
//
// Observable because `emitDisposals` unwinds LIFO
// (src/codegen/LlvmBackend_Disposal.cpp:8007, `for (auto it =
// scope.rbegin(); …)`) and `lowerIEnumerableBuiltinFor` registers the
// enumerator clone first (`LlvmBackend_Stmts.cpp:9821-9826`) and the loop-owned
// source second (`:9899-9902`) — so the OWNER is reclaimed FIRST and the clone's
// dispatch becomes a read of freed memory. `NamedSource…` below is the ordering
// boundary: binding the source to a name inverts the registration order, the
// clone goes first, and P is still live when the clone touches it.
//
// §28.9 IS DECISIVE ON ITS OWN: an instance that does not own the source may
// not dispose it. No spec question is filed — the rule is already encoded in
// this very function as struct slot 3.
//
// ORACLE. `runAotChecked`, ALWAYS ASan, nonzero exit on ANY finding. The PLAIN
// leg is UNRELIABLE here — MEASURED on the pre-fix tree at this file's parent
// (r30-c1 @ a3065a15), `build/bezelc -o` three-to-six times per program:
//
//   * the issue's MIRROR1 spelling: `s=6`, exit 0, six times out of six —
//     SILENT;
//   * the fresh-`GetEnumerator` spelling: `s=6`, exit 0, three of three —
//     SILENT;
//   * the single-implementer spelling: `s=6`, exit 0, three of three —
//     SILENT.
//
// All three would be vacuously green on a plain `runAot` row, so every
// defective row below asserts under ASan. There is no leak to amplify: the
// 24-byte object is freed exactly once, by W₀, correctly — the harm is the
// READ after, and ASan reports it on the first pass (no `Direct leak` line in
// any run). `runAotChecked` reports all four toolchain counters at zero; the
// bare `NATIVE SANITIZER: 0 check(s) skipped` line is an always-printed legend
// and is not a status.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// The self-returning class of the issue's MIRROR1, verbatim: it DECLARES the
// enumerator protocol itself, so the structural scan picks `enumTs == srcTs`,
// and its `GetEnumerator` returns `this`.
const char* kSelfA =
    "public class SelfA: IEnumerable<int>, IEnumerator<int> "
    "  private cur: int := 0; private hi: int; "
    "  public constructor(hi: int) this.hi := hi; end constructor; "
    "  public method GetEnumerator(): IEnumerator<int> return this; end GetEnumerator; "
    "  public method MoveNext(): bool cur := cur + 1; return cur <= hi; end MoveNext; "
    "  public property Current: int get => cur; end Current; "
    "end SelfA; ";

// The issue's "second implementer". The diagnosis MEASURED that this class is
// irrelevant to the defect; the `single_implementer_…` row pins that, and this
// one pins the issue's own spelling.
const char* kSecondImplementer =
    "public class SelfB: IEnumerator<int> "
    "  private cur: int := 0; "
    "  public constructor() end constructor; "
    "  public method MoveNext(): bool cur := cur + 1; return cur <= 3; end MoveNext; "
    "  public property Current: int get => cur; end Current; "
    "end SelfB; ";

// The alias-free twin of `SelfA`: identical in every other respect, but
// `GetEnumerator` returns a FRESH instance of its OWN type. This is the row
// that distinguishes "an `ownsSrc` gate" from "a special case for the
// self-returning alias" — a fix written against the alias would leave this red.
const char* kFreshSelf =
    "public class FreshSelf: IEnumerable<int>, IEnumerator<int> "
    "  private cur: int := 0; private hi: int; "
    "  public constructor(hi: int) this.hi := hi; end constructor; "
    "  public method GetEnumerator(): IEnumerator<int> "
    "    return new FreshSelf(hi); "
    "  end GetEnumerator; "
    "  public method MoveNext(): bool cur := cur + 1; return cur <= hi; end MoveNext; "
    "  public property Current: int get => cur; end Current; "
    "end FreshSelf; ";

// The two-class idiom: `enumTs != srcTs`, so slot 1 holds a GENUINELY distinct
// enumerator class instance. MUST STAY GREEN ON BOTH SIDES. A fix that moved
// the `enumDisposeFn(enumPtr)` arm — rather than gating the
// `srcIsOwnEnumerator` arm — would break exactly this row.
const char* kTwoClass =
    "public class RngEnum: IEnumerator<int> "
    "  private lo: int; private hi: int; private cur: int; "
    "  public constructor(lo: int, hi: int) "
    "    this.lo := lo; this.hi := hi; cur := lo - 1; "
    "  end constructor; "
    "  public method MoveNext(): bool cur := cur + 1; return cur <= hi; end MoveNext; "
    "  public property Current: int get => cur; end Current; "
    "end RngEnum; "
    "public class Rng: IEnumerable<int> "
    "  private lo: int; private hi: int; "
    "  public constructor(lo: int, hi: int) this.lo := lo; this.hi := hi; end constructor; "
    "  public method GetEnumerator(): IEnumerator<int> "
    "    return new RngEnum(lo, hi); "
    "  end GetEnumerator; "
    "end Rng; ";

// A holder with a computed `IEnumerable<int>` PROPERTY is DELIBERATELY ABSENT
// from this file. It is a DIFFERENT defect and is filed separately: a
// hand-written getter `get => _a` reads an `owned` field through a LEND
// (§28.9, "accessor faces are uniform"), but `wrapAsIEnumerableForTarget`
// computes `srcIsNamedOwner` from `ctx_.exprSymbol(*srcExpr)`
// (LlvmBackend_Assign.cpp:9719-9731), which sees no Local/Parameter/Field
// symbol behind a getter body and no auto-property storage symbol either — so it
// mints the wrapper with `ownsSrc = true` and the wrapper FREES a field the
// holder still owns. MEASURED at this file's parent: the use-after-free is
// reported in `bz$M6382D.Holder$DisposeCore`, freed by
// `bz$userenumenum$SelfA$SelfA$Dispose` — the ORIGINAL wrapper reclaiming,
// not a clone disposing a foreign source, and #6382's edit (A) does not and
// must not change it (the flag is read correctly there; its VALUE is wrong
// upstream). Filing it separately is what keeps this file's rows a clean
// before/after signal for #6382 alone.

// Every row returns 0 and prints `s=6`, so `exitCode == 0` is a REAL assertion
// on a run that genuinely ran: `runAotChecked` exits NONZERO on any sanitizer
// finding, and ASan is what carries this defect.
JitResult checked(const std::string& prog) { return runAotChecked(prog); }

}  // namespace

// ===========================================================================
// THE PINNING ROW — the issue's `MIRROR1` verbatim, including the second
// implementer the diagnosis showed to be irrelevant.
//
// MEASURED at this fix's parent, `build/bezelc --sanitize=address` on the same
// program: `heap-use-after-free on address …, READ of size 1`, `#0
// bz$M6382.SelfA$Dispose` (the disposed-flag load at the head of Dispose),
// `#1 bz$userenumenum$SelfA$SelfA$Dispose`, `freed by thread T0 here: #0 free
// / #1 bz$userenumenum$SelfA$SelfA$Dispose`, `previously allocated by … #1
// bz$M6382.M6382Impl$MakeA`; `probe run exit=1`.
//
// The plain leg is silent: six consecutive `bezelc -o` runs print `s=6` and
// exit 0. Hence `runAotChecked` and not `runAot`.
// ===========================================================================
BEZEL_TEST(wrap_clone_disposes_foreign_source_6382,
           issue_mirror1_self_returning_unnamed_ienumerable_source_spec_28_9) {
    JitResult r = checked(
        "module M6382; " + std::string(kSelfA) + kSecondImplementer +
        "public class M6382Impl "
        "  public static method MakeA(): IEnumerable<int> return new SelfA(3); end MakeA; "
        "  public static method Main(): int "
        "    var s := 0; "
        "    for var x in MakeA() do s := s + x; end; "
        "    Console.WriteLine($\"s={s}\"); "
        "    return 0; "
        "  end Main; "
        "end M6382Impl;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // 1 + 2 + 3 — the loop genuinely ran, so a body that never executed
    // cannot make this row vacuously clean.
    BEZEL_EXPECT_TRUE(r.stdoutText.find("s=6") != std::string::npos);
}

// THE ALIAS-FREE ROW. Byte-identical to the pinning row except that
// `GetEnumerator` returns a FRESH instance of its own type, so `enumPtr !=
// srcObj`. This is what rules out the issue's "`enumPtr == srcPtr`" reading as
// the trigger, and it is what a fix written against the ALIAS (rather than
// against ownership) would leave red. It is also the row that shows the clone's
// OWN enumerator must still be disposed — see the comment on the fix.
BEZEL_TEST(wrap_clone_disposes_foreign_source_6382,
           fresh_getenumerator_clone_owns_only_its_own_enumerator_spec_28_9) {
    JitResult r = checked(
        "module M6382B; " + std::string(kFreshSelf) +
        "public class M6382BImpl "
        "  public static method MakeA(): IEnumerable<int> return new FreshSelf(3); end MakeA; "
        "  public static method Main(): int "
        "    var s := 0; "
        "    for var x in MakeA() do s := s + x; end; "
        "    Console.WriteLine($\"s={s}\"); "
        "    return 0; "
        "  end Main; "
        "end M6382BImpl;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("s=6") != std::string::npos);
}

// THE SINGLE-IMPLEMENTER ROW. There is no second implementer of the enumerator
// interface anywhere in this program. The issue argues the second implementer
// matters (that it keeps the structural scan's devirtualization from firing);
// this row pins that the argument is false for this defect. Red at the parent.
BEZEL_TEST(wrap_clone_disposes_foreign_source_6382,
           single_implementer_reproduces_no_second_implementer_needed) {
    JitResult r = checked(
        "module M6382C; " + std::string(kSelfA) +
        "public class M6382CImpl "
        "  public static method MakeA(): IEnumerable<int> return new SelfA(3); end MakeA; "
        "  public static method Main(): int "
        "    var s := 0; "
        "    for var x in MakeA() do s := s + x; end; "
        "    Console.WriteLine($\"s={s}\"); "
        "    return 0; "
        "  end Main; "
        "end M6382CImpl;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("s=6") != std::string::npos);
}

// THE COMPUTED-PROPERTY ROW IS NOT HERE. See the `kHolder` note above: it is a
// distinct defect (the `ownsSrc` PRODUCER is wrong on the hand-written-getter
// path), it stays red under this fix, and shipping it here would turn this
// file's before/after into a mixed signal.

// ---------------------------------------------------------------------------
// THE CONTROLS. Green on BOTH sides of the fix. These are the only thing that
// can catch a wrong-DIRECTION fix, because no pre-existing test covers the
// defective shape at all — every `forin` / `ienumerable` / `iter_` /
// `enumerable` / `enumerator` / `getenumerator` / `wrapper` / `user_class` row
// is green on the broken tree.
// ---------------------------------------------------------------------------

// THE TWO-CLASS CONTROL. `enumTs != srcTs`, so slot 1 holds a distinct
// enumerator class instance and `enumDisposeFn(enumPtr)` is the correct
// dispatch — the arm the fix must NOT touch.
BEZEL_TEST(wrap_clone_disposes_foreign_source_6382,
           two_class_idiom_control_stays_green_spec_29_11) {
    JitResult r = checked(
        "module M6382E; " + std::string(kTwoClass) +
        "public class M6382EImpl "
        "  public static method MakeR(): IEnumerable<int> return new Rng(1, 3); end MakeR; "
        "  public static method Main(): int "
        "    var s := 0; "
        "    for var x in MakeR() do s := s + x; end; "
        "    Console.WriteLine($\"s={s}\"); "
        "    return 0; "
        "  end Main; "
        "end M6382EImpl;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("s=6") != std::string::npos);
}

// THE NAMED-SOURCE CONTROL — the ORDERING BOUNDARY. Binding the source to a
// name inverts the LIFO unwind (the clone is registered after the source, so it
// is disposed FIRST), which is why this spelling was clean even while the
// ownership violation was live. A fix must keep it green.
BEZEL_TEST(wrap_clone_disposes_foreign_source_6382,
           named_source_control_stays_green_spec_28_9) {
    JitResult r = checked(
        "module M6382F; " + std::string(kSelfA) +
        "public class M6382FImpl "
        "  public static method MakeA(): IEnumerable<int> return new SelfA(3); end MakeA; "
        "  public static method Main(): int "
        "    var s := 0; "
        "    var e := MakeA(); "
        "    for var x in e do s := s + x; end; "
        "    Console.WriteLine($\"s={s}\"); "
        "    return 0; "
        "  end Main; "
        "end M6382FImpl;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("s=6") != std::string::npos);
}

// THE `using var` CONTROL — the same boundary by a different spelling: an
// explicitly-scoped owner keeps the source alive past the loop.
BEZEL_TEST(wrap_clone_disposes_foreign_source_6382,
           using_var_control_stays_green_spec_28_9) {
    JitResult r = checked(
        "module M6382G; " + std::string(kSelfA) +
        "public class M6382GImpl "
        "  public static method MakeA(): IEnumerable<int> return new SelfA(3); end MakeA; "
        "  public static method Main(): int "
        "    var s := 0; "
        "    using var e := MakeA(); "
        "    for var x in e do s := s + x; end; "
        "    Console.WriteLine($\"s={s}\"); "
        "    return 0; "
        "  end Main; "
        "end M6382GImpl;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("s=6") != std::string::npos);
}

// THE CLASS-TYPED-SOURCE CONTROL — the #6378 NEIGHBOURHOOD, and the row that
// proves the two defects are DISJOINT. Here the source's static type is the
// CLASS, not the declaration-less builtin `IEnumerable<T>` sentinel, so
// `lowerIEnumerableFor` DOES run and `enumIsTsOrBase` IS true. MEASURED
// ASan-clean at the parent's parent too: it is green before and green after,
// which is why "the builtin sentinel is the trigger" is a measurement and not
// an inference.
BEZEL_TEST(wrap_clone_disposes_foreign_source_6382,
           class_typed_source_control_stays_green_spec_29_11) {
    JitResult r = checked(
        "module M6382H; " + std::string(kSelfA) +
        "public class M6382HImpl "
        "  public static method Main(): int "
        "    var s := 0; "
        "    for var x in new SelfA(3) do s := s + x; end; "
        "    Console.WriteLine($\"s={s}\"); "
        "    return 0; "
        "  end Main; "
        "end M6382HImpl;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("s=6") != std::string::npos);
}