// Codegen — #6343 / §29.1 "The protocol" / §29.11 / §11.2 / §12.1 (v0.338).
//
// THE ISSUE. Two MANDATORY oracles disagreed SILENTLY on the same program:
// the plain AOT leg printed `9`, the ASan leg printed `0` — exit code 0 both
// ways, and NO AddressSanitizer report at all. Independently measured twice by
// other agents, at base and at tip, so pre-existing and not a regression of any
// recent fix.
//
// ROOT CAUSE — a TYPE CONFUSION in `wrapUserClassAsIEnumerable`
// (LlvmBackend_Assign.cpp), which adapts a user class that does not itself
// implement the enumerator protocol into the §29.11 iterator-vtable format.
//
// When the source class is not self-implementing, that function picks
// `enumTs` — the concrete enumerator class — by a STRUCTURAL SCAN over every
// compiled class looking for MoveNext + Current. That guess is correct for the
// two-class idiom (`IntRange.GetEnumerator()` really does `return new
// IntRangeEnum(...)`) and WRONG for a source whose `GetEnumerator`
// DELEGATES:
//
//     public class Bag
//       public method GetEnumerator(): owned IEnumerator<int>
//         => _c.GetEnumerator();          // _c is a Counter
//     end Bag;
//
// The eagerly fetched `enumPtr` (step 9 of that function) is then NOT a
// `Counter` instance — it is a value the callee already put in §29.11
// iterator-vtable format. `wrapMoveNextFn` / `wrapCurrentFn` /
// `wrapDisposeFn` are typed against `Counter`'s FIELD LAYOUT, so calling
// `Counter.MoveNext` on it reads `_cur` at the byte offset `_cur` occupies in a
// real `Counter` — which lands inside the wrapper struct's own `enumerator`
// POINTER field, reading bits 32..47 of a heap address.
//
// MEASURED, and the mechanism is layout, not protocol (IR dumped with
// BEZEL_DUMP_IR; `bz$userenumenum$Bag$Counter$MoveNext` emits
// `call i1 @bz$Counter$MoveNext(ptr %enumPtr)` where `%enumPtr` is a
// `{ptr vtable, ptr enumerator, ptr source, i1}` and a `Counter` is
// `{ptr vtable, i8 disposed, i32 _cur}`, so `_cur` is read at byte 12):
//
//   field layout        plain oracle    ASan oracle
//   {ptr,i8,i32}            9               0            <- bits 32..47: 0 vs 0x7b7e
//   {ptr,i8,i32,i32}        0              OverflowException
//
// The second row inserts one `int` ahead of `_cur`, moving the read from byte
// 12 to byte 16; BOTH oracles then agree on a different wrong answer, and the
// ASan leg throws where the plain leg exits 0. So the divergence is a value
// that depends only on WHERE THE OBJECT LANDS — which is precisely why neither
// oracle can see it: every byte read is in bounds of a live heap block.
//
// THE FIX. The contract for `enumPtr` is not "an instance of the guessed
// class"; it is §29.11's own — a `GetEnumerator()` whose declared return is
// the `IEnumerator<T>` / `IOwningEnumerator<T>` head yields an
// `IEnumerator<T>`-FORMAT value. So when `enumPtr`'s slot-0 vtable is not
// `enumTs`'s own class vtable, the value is ALREADY in the format the caller
// consumes and must be driven THROUGH ITS OWN VTABLE, not through the guessed
// class's methods. One runtime identity test selects a second `$ad` wrapper
// family whose Dispose/MoveNext/Current re-dispatch through `enumPtr`'s slots
// 1/2/3; every wrapper that does not need it keeps byte-identical generated
// code.
//
// WHY EVERY PINNING ROW BELOW IS GREEN ON BOTH ORACLES AND FAILS WITHOUT THE
// FIX. On the PLAIN leg the confused read returned 0 by luck, so a plain row
// alone would not fail — the plain rows here are companions, not the proof. The
// `runAotChecked` rows are the proof: they read the same program's value under
// the ASan leg, which returned 0 before the fix and returns 9 after.
//
// NOT COVERED HERE, DELIBERATELY: the issue's own repro holds the delegating
// `Counter` in a LOCAL of `Bag.GetEnumerator` rather than in a field. There
// `var c := new Counter();` is an owned local disposed at that method's scope
// end while the returned enumerator still aliases it, so the enumerator DANGLES
// — a heap-use-after-free that this fix's better diagnosis EXPOSES (ASan
// reports it) rather than causes (pre-fix, the confused read touched the
// wrapper's own bytes, never `c`, so ASan stayed silent and the value came out
// as 0). Whether §29.1's "the caller takes ownership of the enumerator" makes
// that `return` a MOVE of `c` — so the scope-end disposal must be suppressed —
// or whether the shape must be rejected, is a spec question and is filed as
// such; no row here pins a value for it.
//
// SPEC BASIS. §29.1 ("GetEnumerator() returns owned IEnumerator<T>: the caller
// takes ownership of the enumerator and must dispose it"), §29.11 ("Each
// GetEnumerator() call produces an independent enumerator"), §11.2 / §12.1
// (a member's return type must be exactly equal to the interface's under
// substitution — which is why a user's `GetEnumerator(): IEnumerator<T>` can
// never name a concrete class, and why the concrete class has to be resolved
// some other way; this fix stops resolving it by an unchecked guess).
// Expected values are computed from the programs themselves: `n := n +
// e.Current + 1` over the enumeration 1, 2, 3 sums to 2 + 3 + 4 = 9.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A class that implements the enumerator protocol by the #2384 self-returning
// idiom. `Counter.GetEnumerator()` returns `this`.
const char* kCounter =
    "public class Counter "
    "  private _cur: int := 0; "
    "  public method GetEnumerator(): IEnumerator<int> return this; end GetEnumerator; "
    "  public method MoveNext(): bool _cur := _cur + 1; return _cur <= 3; end MoveNext; "
    "  public property Current: int get => _cur; end Current; "
    "end Counter; ";

// The DELEGATING source. `_c` is an `owned` FIELD, so the Counter's lifetime
// is Bag's, not the GetEnumerator method's — which keeps this row about the
// wrapper's typing alone, with no lifetime question mixed in.
const char* kDelegatingBag =
    "public class Bag "
    "  private owned _c: Counter := new Counter(); "
    "  public method GetEnumerator(): owned IEnumerator<int> "
    "    => _c.GetEnumerator(); "
    "end Bag; ";

// The consuming driver, shared by every delegating row: `n` accumulates
// `Current + 1` over the enumeration 1, 2, 3, so a correct run prints 9 and an
// empty or mis-typed enumerator prints 0. stdout rather than the exit code, so
// the value is asserted without relying on an 8-bit exit-code truncation.
std::string delegatingProgram(const char* moduleName, const char* extras = "") {
    return std::string("module ") + moduleName + "; " + extras + kCounter +
           kDelegatingBag +
           "public class M "
           "  public static method Main(): int "
           "    var b := new Bag(); "
           "    using var e := b.GetEnumerator(); "
           "    var n := 0; "
           "    while e.MoveNext() do n := n + e.Current + 1; end; "
           "    Console.WriteLine($\"{n}\"); "
           "    return 0; "
           "  end Main; end M;";
}

// The `run()`/`runChecked()` pair, matching iter_selfreturning_wrap_2384_test.cpp:
// `runChecked` is generated-code ASan + leak detection, `run` is the plain leg.
int run(const std::string& prog) {
    JitResult r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
std::string runOut(const std::string& prog) {
    JitResult r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    return r.stdoutText;
}
JitResult checked(const std::string& prog) { return runAotChecked(prog); }

}  // namespace

// §29.11 / §29.1: THE PINNING ROW. `Bag.GetEnumerator()` returns an
// `IEnumerator<T>`-format value built by `_c.GetEnumerator()`; the wrapper
// must drive that value through ITS OWN vtable. Pre-fix, `bz$Counter$MoveNext`
// was called on the wrapper struct and `_cur` came back as bits 32..47 of a
// heap address — 0 under glibc malloc (so the PLAIN leg printed 9 by accident)
// and 0x7b7e under the ASan allocator (so the ASan leg printed 0). The ASan
// leg is therefore the oracle that sees this defect, and it is the oracle that
// proves the fix.
BEZEL_TEST(codegen_user_enumerator_delegating_wrapper_6343,
           delegating_getenumerator_yields_all_elements_spec_29_11) {
    JitResult r = checked(delegatingProgram("M6343A"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("9") != std::string::npos);
}

// The same program on the PLAIN leg. Green before and after the fix (the
// confused read returned 0 by luck under glibc malloc), so this row is a
// companion pinning the plain-oracle value rather than the proof — it exists so
// a future change cannot make the plain leg disagree with the checked leg
// again, which is the failure mode this issue is about.
BEZEL_TEST(codegen_user_enumerator_delegating_wrapper_6343,
           delegating_getenumerator_plain_leg_agrees_spec_29_11) {
    BEZEL_EXPECT_TRUE(runOut(delegatingProgram("M6343B")).find("9") !=
                      std::string::npos);
}

// THE SHARPEST FORM OF THE DEFECT: an enumerator-shaped class that has nothing
// to do with the delegation is declared in the module, which is all it takes
// for the structural scan to select it as `Bag`'s enumerator class and drive
// ITS methods over `_c`'s adapter. `OtherEnum` enumerates 100, 101, 102 — if
// the scan's guess were ever honoured, the row would print 300-something
// rather than 9. Pre-fix this program printed `9` plain / `0` under ASan, for
// exactly the reason above; the row pins the value that is correct per §29.1.
BEZEL_TEST(codegen_user_enumerator_delegating_wrapper_6343,
           unrelated_enumerator_class_in_module_cannot_retype_the_wrapper_6343) {
    const char* kOther =
        "public class OtherEnum: IEnumerator<int> "
        "  private _k: int := 99; "
        "  public constructor() _k := 99; end constructor; "
        "  public method MoveNext(): bool _k := _k + 1; return _k <= 3; end MoveNext; "
        "  public property Current: int get => _k; end Current; "
        "end OtherEnum; ";
    JitResult r = checked(delegatingProgram("M6343C", kOther));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("9") != std::string::npos);
}

// THE OTHER DELEGATION TARGET: a builtin collection's enumerator. `Bag` holds
// the array in a plain (non-`owned`) field fed from a constructor parameter
// rather than in an `owned` field initialised from a literal, precisely because
// an `Array` FIELD's backing store is never reclaimed (#6341, filed separately
// and independent of this defect — it reproduces with no enumerator in the
// program). This row is an ASan row, so that leak would otherwise be
// attributed here; measured LSan-clean in this shape.
//
// Pre-fix this is #6326's own program PLUS one unrelated enumerator-shaped
// class: with no such class the wrap DECLINES and #6326's decline fix returns
// the callee's enumerator correctly (9), but WITH one the structural scan
// succeeds, the wrap is built over the callee's already-correct Array
// enumerator, and `OtherEnum`'s field layout is read off it — `9` plain, `0`
// under ASan. Same defect, same fix, and it pins that adding an unrelated
// class declaration to a module can no longer change the program's answer.
BEZEL_TEST(codegen_user_enumerator_delegating_wrapper_6343,
           array_delegating_getenumerator_with_unrelated_class_6343) {
    const char* kOther =
        "public class OtherEnum: IEnumerator<int> "
        "  private _k: int := 99; "
        "  public constructor() _k := 99; end constructor; "
        "  public method MoveNext(): bool _k := _k + 1; return _k <= 3; end MoveNext; "
        "  public property Current: int get => _k; end Current; "
        "end OtherEnum; ";
    const std::string prog =
        "module M6343D; " + std::string(kOther) +
        "public class Bag "
        "  private _a: Array<int>; "
        "  public constructor(a: Array<int>) _a := a; end constructor; "
        "  public method GetEnumerator(): owned IEnumerator<int> "
        "    => _a.GetEnumerator(); "
        "end Bag; "
        "public class M "
        "  public static method Main(): int "
        "    var a: Array<int> := [1, 2, 3]; "
        "    var b := new Bag(a); "
        "    using var e := b.GetEnumerator(); "
        "    var n := 0; "
        "    while e.MoveNext() do n := n + e.Current + 1; end; "
        "    Console.WriteLine($\"{n}\"); "
        "    return 0; "
        "  end Main; end M;";
    JitResult r = checked(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // 1, 2, 3 -> (1+1)+(2+1)+(3+1) = 9. An empty or mis-typed enumerator
    // yields 0, and the mis-typed one read a heap address here.
    BEZEL_EXPECT_TRUE(r.stdoutText.find("9") != std::string::npos);
}

// #2424's `ownsSrc` half ON THE `$ad` PATH. `new Bag()` is an UNNAMED temporary
// receiver, so nothing else in the program disposes it — the wrapper built by
// `wrapUserClassAsIEnumerable` is its only disposer, and its slot-3 `ownsSrc`
// flag must therefore still reclaim it. This is the case the `$ad` family could
// most easily have lost: the `$ad` Dispose hands the INNER adapter's
// reclamation to that adapter's own Dispose, so it must still run the `ownsSrc`
// half for `srcObj` separately, and must not have started freeing `enumPtr`.
//
// The inner `Bag` also owns a `Counter` in an `owned` field, so a `$ad` Dispose
// that dispatched `srcDisposeFn` twice — once via the inner wrapper's chain and
// once via its own `ownsSrc` branch — would double-dispose and double-reclaim
// it. Measured LSan-clean and heap-error-free in this shape. 1 + 2 + 3 = 6.
BEZEL_TEST(codegen_user_enumerator_delegating_wrapper_6343,
           temporary_receiver_is_disposed_once_through_the_adaptive_wrapper_6343) {
    JitResult r = checked(
        "module M6343G; " + std::string(kCounter) +
        "public class Bag "
        "  private owned _c: Counter := new Counter(); "
        "  public method GetEnumerator(): owned IEnumerator<int> "
        "    return _c.GetEnumerator(); "
        "  end GetEnumerator; "
        "end Bag; "
        "public class M "
        "  public static method Main(): int "
        "    var n := 0; "
        "    using var e := new Bag().GetEnumerator(); "
        "    while e.MoveNext() do n := n + e.Current; end; "
        "    Console.WriteLine($\"{n}\"); "
        "    return 0; "
        "  end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("6") != std::string::npos);
}

// CONTROL — the DIRECT family is untouched. The two-class idiom is the shape
// the structural scan was written FOR: `IntRange.GetEnumerator()` really does
// `return new IntRangeEnum(...)`, so `enumPtr`'s slot-0 vtable IS
// `vt$IntRangeEnum` and the identity test selects the original, byte-identical
// lowering. Pinned under ASan: if the new `$ad` family were selected here the
// direct dispatch through `IntRangeEnum`'s own CG35 vtable slot 2 (`Dispose`,
// not `MoveNext`) would be called and the answer would not be 6.
BEZEL_TEST(codegen_user_enumerator_delegating_wrapper_6343,
           two_class_idiom_still_takes_the_direct_family_spec_29_11) {
    BEZEL_EXPECT_EQ(run(
        "module M6343E; "
        "class IntRangeEnum: IEnumerator<int> "
        "  private lo: int; private hi: int; private cur: int; "
        "  public constructor(lo: int, hi: int) this.lo:=lo; this.hi:=hi; cur:=lo-1; end constructor; "
        "  public method MoveNext(): bool cur:=cur+1; return cur<=hi; end MoveNext; "
        "  public property Current: int get => cur; end Current; "
        "end IntRangeEnum; "
        "class IntRange: IEnumerable<int> "
        "  private lo: int; private hi: int; "
        "  public constructor(lo: int, hi: int) this.lo:=lo; this.hi:=hi; end constructor; "
        "  public method GetEnumerator(): IEnumerator<int> return new IntRangeEnum(lo, hi); end GetEnumerator; "
        "end IntRange; "
        "public class M "
        "  public static method Main(): int "
        "    using var r := new IntRange(1, 3); "
        "    var s := 0; for var x in r do s := s + x; end; "
        "    Console.WriteLine($\"{s}\"); "
        "    return 0; "
        "  end Main; end M;"), 0);
}

// CONTROL — the SELF-RETURNING idiom is untouched. `SelfIter` implements BOTH
// interfaces and returns `this`, so `structurallyIsEnumerator(srcTs)` is true,
// `enumTs == srcTs`, and `enumPtr == srcObj` whose slot-0 vtable IS
// `vt$SelfIter` — the identity test selects the direct family here too. The
// ALIASING result is the class's own documented choice (§29.11 satisfied at the
// wrapper level; see iter_selfreturning_wrap_2384_test.cpp's spec reading), not
// a compiler artifact, so this row pins it: 1*10 + 2 = 12.
BEZEL_TEST(codegen_user_enumerator_delegating_wrapper_6343,
           self_returning_idiom_still_takes_the_direct_family_spec_29_11) {
    BEZEL_EXPECT_TRUE(runOut(
        "module M6343F; "
        "public class SelfIter: IEnumerable<int>, IEnumerator<int> "
        "  private cur: int := 0; private hi: int; "
        "  public constructor(hi: int) this.hi := hi; end constructor; "
        "  public method GetEnumerator(): IEnumerator<int> return this; end GetEnumerator; "
        "  public method MoveNext(): bool cur := cur + 1; return cur <= hi; end MoveNext; "
        "  public property Current: int get => cur; end Current; "
        "end SelfIter; "
        "public class M "
        "  public static method Main(): int "
        "    var s: IEnumerable<int> := new SelfIter(3); "
        "    using var e1 := s.GetEnumerator(); "
        "    var a := 0; "
        "    if e1.MoveNext() then a := e1.Current; end; "
        "    using var e2 := s.GetEnumerator(); "
        "    var b := 0; "
        "    if e2.MoveNext() then b := e2.Current; end; "
        "    Console.WriteLine($\"{a * 10 + b}\"); "
        "    return 0; "
        "  end Main; end M;").find("12") != std::string::npos);
}