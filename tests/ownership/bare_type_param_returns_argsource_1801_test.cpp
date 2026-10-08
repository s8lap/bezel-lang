// Spec-derived tests for the §28.3 *Bare-type-parameter returns* keystone
// (v0.194, #1801 sub-tasks S3/S4/S5/S10/S11/S13) — the CALLER-side
// argument-source-union rules that compose on top of S1's union-source attach.
//
// §28.3 "Bare-type-parameter returns" — Caller side (v0.194): "the call produces
// a **borrow** carrying the **union of the source-liveness constraints** of the
// receiver and of every argument bound to a source parameter ... A borrowed
// argument contributes its own carried constraints transitively; an argument that
// is an **unbound owned temporary** (a fresh producer, a `move` result ...) bounds
// the result borrow to the same full expression, and a binding, store, return, or
// other escape of the result past that expression is rejected."
//
// Migration note (v0.194): "`using var m := Math.Max(a, b);` becomes a §28.10
// borrow-pin"; "disposing/`_ :=`-discarding a `Math.Max` result at class T ... is
// newly rejected — each was a latent double-owner"; "Loop-carried accumulation of
// a bare-T call result (`prev := Math.Max(prev, cur)` with per-iteration `cur`) is
// rejected by the existing v0.180 back-edge bullet". Inheritance (v0.194): "a
// method implementing or overriding a generic declaration whose return type is a
// bare type parameter carries this classification at every call site, direct or
// dispatched ... the potential source set is read from the **generic
// declaration's** signature". Intrinsics: `Math.Min`/`Max`/`Clamp` (stdlib §2.2)
// borrow their operands; `Aggregate<TAccum>(seed, fold)` (stdlib §5) borrows
// {receiver, seed}.
//
// SCOPE:
//   S3  — reject laundering a fresh owned temporary out through the borrow result.
//   S4  — a `using var` over the borrow pins it (accepted, no owning-binding leak
//         warning); an explicit `.Dispose()` of the borrow is rejected.
//   S5  — reject loop-carried accumulation whose source dies each iteration.
//   S10 — a DIRECT call on a concrete implementer/overrider inherits the
//         classification (source set read positionally from the base declaration).
//   S11 — `Math.Min`/`Max`/`Clamp` results borrow their operands.
//   S13 — `Aggregate(seed, fold)` result borrows {receiver, seed}.

#include <string>
#include <string_view>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

bool errorSays(const OwnershipResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

// A class C with a comparable surface, plus a static owned-argument sink used to
// consume a source while the bare-T borrow result is live.
constexpr std::string_view kPrelude =
    "module M; "
    "class C "
    "  public constructor() end constructor; "
    "  destructor end destructor; "
    "  public method Poke(): int return 1; end Poke; "
    "  public method Cmp(o: C): int return 0; end Cmp; "
    "end C; "
    "class Util "
    "  public static method Pick<T>(a: T, b: T): T return a; end Pick; "
    "  public static method Identity<T>(a: T): T return a; end Identity; "
    "  public static method Sink(owned c: C): int c.Dispose(); return 0; "
    "  end Sink; "
    "end Util; ";

std::string prog(std::string_view body) {
    return std::string(kPrelude) +
           "class Prog "
           "  public static method Main(): int " +
           std::string(body) +
           "  end Main; "
           "end Prog;";
}

}  // namespace

// ===========================================================================
// S3 — fresh-argument laundering. An unbound owned temporary argument bounds the
// bare-T borrow result to the full expression; binding it is a launder escape.
// ===========================================================================

// REJECT: `var m := Identity(new C());` — the argument `new C()` is a fresh owned
// temporary disposed at the end of the statement, and `m` borrows it, so binding
// `m` lets the borrow escape its source. FAILS WITHOUT THE FIX (compiled clean —
// the laundered temporary silently dangled). // §28.3 v0.194 unbound-temp rule
BEZEL_TEST(bare_type_param_returns_argsource,
           s3_bind_fresh_temp_argument_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(prog(
        "var m := Util.Identity(new C()); "
        "var p := m.Poke(); "
        "return p; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "unbound owned temporary"));
}

// REJECT via RETURN: `return Util.Identity(new C());` launders the fresh owned
// temporary out through the borrow result too. (An instance method so the return
// is not first rejected by the static-borrow-return rule.)
BEZEL_TEST(bare_type_param_returns_argsource,
           s3_return_fresh_temp_argument_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(
        std::string(kPrelude) +
        "class Prog "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "  public method Make(): C return Util.Identity(new C()); end Make; "
        "  public static method Main(): int return 0; end Main; "
        "end Prog;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "unbound owned temporary"));
}

// ACCEPT (over-reject guard): `var m := Identity(a);` over a BOUND owned local
// `a` — not a temporary; S1's consume-freeze governs its liveness, and binding
// `m` is admitted (the source outlives the uses here). Must NOT trip the S3
// launder reject.
BEZEL_TEST(bare_type_param_returns_argsource,
           s3_bind_bound_local_argument_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(prog(
        "var a := new C(); "
        "var m := Util.Identity(a); "
        "var p := m.Poke(); "
        "a.Dispose(); "
        "return p; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// S4 — the result is a BORROW: a `using var` over it pins (no owning binding),
// and an explicit `.Dispose()` of it is rejected (you do not own it).
// ===========================================================================

// REJECT: `m.Dispose()` on a bare-T borrow result — the caller does not own it,
// so disposing it would free a value a source still owns (a latent double-owner).
// FAILS WITHOUT THE FIX (a `.Dispose()` on the Borrowed holder silently marked it
// Disposed). // §28.3 v0.194 migration note (newly rejected)
BEZEL_TEST(bare_type_param_returns_argsource,
           s4_explicit_dispose_of_borrow_result_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(prog(
        "var a := new C(); "
        "var b := new C(); "
        "var m := Util.Pick(a, b); "
        "m.Dispose(); "
        "a.Dispose(); "
        "b.Dispose(); "
        "return 0; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        errorSays(r, "cannot call `Dispose()` on a bare-type-parameter call "
                     "result"));
}

// ACCEPT: `using var m := Util.Pick(a, b);` is a §28.10 borrow-pin, not an owning
// binding — accepted with no owning-binding leak/auto-drop error (its sources are
// disposed by their real owners). The pin classification prevents the double-free
// codegen would otherwise emit.
BEZEL_TEST(bare_type_param_returns_argsource,
           s4_using_var_over_borrow_pins_accepted_spec_28_10) {
    auto r = parseAndCheckOwnership(prog(
        "var a := new C(); "
        "var b := new C(); "
        "using var m := Util.Pick(a, b); "
        "var p := m.Poke(); "
        "a.Dispose(); "
        "b.Dispose(); "
        "return p; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// S5 — loop-carried accumulation whose source does not outlive the back edge.
// ===========================================================================

// REJECT: `prev := Util.Pick(prev, cur);` inside a loop where `cur` is declared in
// the loop body. `prev` (the accumulator, at the loop's enclosing depth) borrows
// the union {prev, cur}, but `cur` is disposed each iteration — a source that does
// not outlive the back edge. FAILS WITHOUT THE FIX (compiled clean; `prev`
// dangled after the loop). // §28.3 v0.194 back-edge bullet
BEZEL_TEST(bare_type_param_returns_argsource,
           s5_loop_carried_accumulation_over_body_local_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(prog(
        "var prev := new C(); "
        "var i := 0; "
        "while i < 3 do "
        "  var cur := new C(); "
        "  prev := Util.Pick(prev, cur); "
        "  cur.Dispose(); "
        "  i := i + 1; "
        "end; "
        "prev.Dispose(); "
        "return 0; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "would outlive its source `cur`"));
}

// ACCEPT (over-reject guard): the accumulation sources `x` and `y` are declared
// OUTSIDE the loop (the same enclosing scope as `prev`), so they outlive every
// iteration — no back-edge escape, accepted. `prev` is a borrow throughout (never
// a source of its own reassignment), so only the source depths decide.
BEZEL_TEST(bare_type_param_returns_argsource,
           s5_loop_accumulation_over_outer_source_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(prog(
        "var x := new C(); "
        "var y := new C(); "
        "var prev := Util.Pick(x, y); "
        "var i := 0; "
        "while i < 3 do "
        "  prev := Util.Pick(x, y); "
        "  i := i + 1; "
        "end; "
        "var p := prev.Poke(); "
        "x.Dispose(); y.Dispose(); "
        "return p; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// S10 — inheritance: a DIRECT call on a concrete implementer of a generic
// bare-T declaration inherits the argument-source-union classification, read
// positionally from the interface declaration.
// ===========================================================================

// REJECT: `vc.Pick(a, b)` where `vc: VChooser` (a class implementing
// `IChooser<C>` whose `Pick` has a CONCRETE `C` return) — the classification is
// inherited from `IChooser<T>.Pick(a: T, b: T): T`, so the result borrows {a, b}
// and consuming `a` while `m` is live freezes. FAILS WITHOUT THE FIX (the impl's
// own concrete signature is not bare-T, so no source set attached). // §28.3
// v0.194 inheritance
BEZEL_TEST(bare_type_param_returns_argsource,
           s10_direct_call_on_impl_inherits_classification_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(
        std::string(kPrelude) +
        "interface IChooser<T> "
        "  method Pick(a: T, b: T): T; "
        "end IChooser; "
        "class VChooser: IChooser<C> "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "  public method Pick(a: C, b: C): C return a; end Pick; "
        "end VChooser; "
        "class Prog "
        "  public static method Main(): int "
        "    using var vc := new VChooser(); "
        "    var a := new C(); "
        "    var b := new C(); "
        "    var m := vc.Pick(a, b); "
        "    _ := Util.Sink(move a); "  // consume `a` while `m` is live
        "    var p := m.Poke(); "
        "    b.Dispose(); "
        "    return p; "
        "  end Main; "
        "end Prog;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "cannot consume `a`"));
    BEZEL_EXPECT_TRUE(errorSays(r, "borrow held by `m`"));
}

// ACCEPT (over-reject guard): the SAME direct impl call, but with `m`'s last use
// BEFORE the consume — the borrow is dead at the consume, so nothing freezes.
BEZEL_TEST(bare_type_param_returns_argsource,
           s10_direct_call_on_impl_dead_before_consume_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(
        std::string(kPrelude) +
        "interface IChooser<T> "
        "  method Pick(a: T, b: T): T; "
        "end IChooser; "
        "class VChooser: IChooser<C> "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "  public method Pick(a: C, b: C): C return a; end Pick; "
        "end VChooser; "
        "class Prog "
        "  public static method Main(): int "
        "    using var vc := new VChooser(); "
        "    var a := new C(); "
        "    var b := new C(); "
        "    var m := vc.Pick(a, b); "
        "    var p := m.Poke(); "            // `m`'s last use precedes the consume
        "    _ := Util.Sink(move a); "
        "    b.Dispose(); "
        "    return p; "
        "  end Main; "
        "end Prog;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// S11 — `Math.Min`/`Max`/`Clamp` results borrow their operands.
// ===========================================================================

// REJECT: `var m := Math.Max(a, b);` yields a borrow with sources {a, b};
// consuming `a` while `m` is live freezes. FAILS WITHOUT THE FIX (the intrinsic
// has no user MethodDecl, so no source set was attached). // §28.3/§2.2 v0.194
BEZEL_TEST(bare_type_param_returns_argsource,
           s11_math_max_result_borrows_operands_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(prog(
        "var a := new C(); "
        "var b := new C(); "
        "var m := Math.Max(a, b); "
        "_ := Util.Sink(move a); "  // consume an operand while `m` is live
        "var p := m.Poke(); "
        "b.Dispose(); "
        "return p; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "cannot consume `a`"));
    BEZEL_EXPECT_TRUE(errorSays(r, "borrow held by `m`"));
}

// REJECT: `Math.Clamp(value, low, high)` borrows all three operands — consuming
// `high` while the result is live freezes.
BEZEL_TEST(bare_type_param_returns_argsource,
           s11_math_clamp_result_borrows_all_operands_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(prog(
        "var value := new C(); "
        "var low := new C(); "
        "var high := new C(); "
        "var m := Math.Clamp(value, low, high); "
        "_ := Util.Sink(move high); "
        "var p := m.Poke(); "
        "value.Dispose(); low.Dispose(); "
        "return p; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "cannot consume `high`"));
}

// ACCEPT (over-reject guard): a VALUE instantiation `Math.Max(1, 2)` at `T = int`
// — the operands are value types, contributing no constraint, so nothing freezes
// and no borrow is tracked.
BEZEL_TEST(bare_type_param_returns_argsource,
           s11_math_max_value_instantiation_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(prog(
        "var x := Math.Max(1, 2); "
        "var y := 5; "
        "return x + y; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// S13 — `Aggregate<TAccum>(seed, fold)` result borrows {receiver, seed}.
// ===========================================================================

// REJECT: `var m := arr.Aggregate(seed, fold);` at class `TAccum` — the result
// borrows the seed (the value returned for an empty source) as well as the
// receiver; consuming `seed` while `m` is live freezes. FAILS WITHOUT THE FIX
// (the intrinsic attached only the receiver, if anything, never the seed). //
// §28.3/§5 v0.194 — the one §5 classification v0.194 tightened
BEZEL_TEST(bare_type_param_returns_argsource,
           s13_aggregate_result_borrows_seed_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(prog(
        "var arr: Array<int> := [1, 2, 3]; "
        "var seed := new C(); "
        "var m := arr.Aggregate(seed, lambda(acc: C, x: int) => acc); "
        "_ := Util.Sink(move seed); "  // consume the seed while `m` is live
        "var p := m.Poke(); "
        "return p; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "cannot consume `seed`"));
    BEZEL_EXPECT_TRUE(errorSays(r, "borrow held by `m`"));
}

// ACCEPT (over-reject guard): the SAME aggregation with the seed consumed AFTER
// the result's last use — the borrow is dead at the consume, nothing freezes.
BEZEL_TEST(bare_type_param_returns_argsource,
           s13_aggregate_seed_consumed_after_last_use_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(prog(
        "var arr: Array<int> := [1, 2, 3]; "
        "var seed := new C(); "
        "var m := arr.Aggregate(seed, lambda(acc: C, x: int) => acc); "
        "var p := m.Poke(); "               // last use of the result
        "_ := Util.Sink(move seed); "  // now safe — the borrow is dead
        "return p; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// #1876 — LINQ ELEMENT-ACCESS + no-selector Min()/Max() at a CLASS element type
// are §28.3 *receiver-sourced borrows* (stdlib §5 "Ownership at class-typed
// substitutions", v0.194): the result borrows the RECEIVER sequence, so a
// consume of the receiver while the result is live freezes (S1), returning /
// laundering the result past the receiver's lifetime is rejected (S2), and
// disposing the result is rejected (S4/§28.6). A value-typed element result
// attaches nothing (§28.9). Members: First/Last/Single/ElementAt (+ *OrNull +
// predicate forms) and no-selector Min()/Max() (§5.1). Each REJECT below FAILS
// WITHOUT THE FIX (the intrinsic had no user MethodDecl, so no source set was
// attached and the escape compiled clean — a standing under-reject).
// // per §28.3 v0.194 / stdlib §5
// ===========================================================================

namespace {

// An owned class collection (`OwningArray<Obj>`), a comparable `Obj` (for the
// no-selector `Min()`/`Max()` §5.1 forms), and owned sinks that consume a
// receiver (a class collection or an int array) while a First/Min borrow is
// live. `Obj` is a class, so `arr.First()`/`arr.Min()` return a class element
// borrow of `arr`; `Array<int>.First()` returns an `int` value (attaches
// nothing).
constexpr std::string_view kLinqPrelude =
    "module M; "
    "class Obj: IComparable<Obj> "
    "  public constructor() end constructor; "
    "  destructor end destructor; "
    "  public method CompareTo(o: Obj): int return 0; end CompareTo; "
    "  public method Poke(): int return 1; end Poke; "
    "end Obj; "
    "class Sink "
    "  public static method Arr(owned a: OwningArray<Obj>): int "
    "    a.Dispose(); return 0; "
    "  end Arr; "
    "  public static method Ints(owned a: Array<int>): int "
    "    a.Dispose(); return 0; "
    "  end Ints; "
    "end Sink; ";

std::string linqProg(std::string_view body) {
    return std::string(kLinqPrelude) +
           "class Prog "
           "  public static method Main(): int " +
           std::string(body) +
           "  end Main; "
           "end Prog;";
}

}  // namespace

// REJECT: `var e := arr.First();` binds a borrow of a receiver element; consuming
// the receiver `arr` (an `owned` transfer into the sink) while `e` has a later
// use is a consume-site freeze. FAILS WITHOUT THE FIX (compiled clean; `e`
// dangled after `arr` was disposed). // §28.3 v0.194 / stdlib §5
BEZEL_TEST(bare_type_param_returns_argsource,
           n1876_first_consume_receiver_freeze_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(linqProg(
        "var arr := new OwningArray<Obj>(); "
        "arr.Add(new Obj()); "
        "var e := arr.First(); "
        "_ := Sink.Arr(move arr); "  // consume the receiver while `e` is live
        "var p := e.Poke(); "
        "return p; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "cannot consume `arr`"));
    BEZEL_EXPECT_TRUE(errorSays(r, "borrow held by `e`"));
}

// ACCEPT (over-reject guard): the SAME shape with the receiver consumed AFTER the
// borrow's last use — the borrow is dead at the consume, nothing freezes.
BEZEL_TEST(bare_type_param_returns_argsource,
           n1876_first_consume_after_last_use_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(linqProg(
        "var arr := new OwningArray<Obj>(); "
        "arr.Add(new Obj()); "
        "var e := arr.First(); "
        "var p := e.Poke(); "             // last use of the borrow
        "_ := Sink.Arr(move arr); "  // now safe — the borrow is dead
        "return p; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// REJECT via RETURN: `var m := arr.Min(); return m;` returns a borrow of the
// local receiver `arr`, which is disposed at scope end — the borrow outlives its
// source. An INSTANCE method, so the return is not first rejected by the
// static-borrow-return rule (#1547). FAILS WITHOUT THE FIX (no source attached;
// `arr` was auto-dropped and the borrow escaped silently). // §28.3 / §5.1 v0.194
BEZEL_TEST(bare_type_param_returns_argsource,
           n1876_min_result_return_escapes_receiver_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(
        std::string(kLinqPrelude) +
        "class Prog "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "  public method GetMin(): Obj "
        "    var arr := new OwningArray<Obj>(); "
        "    arr.Add(new Obj()); "
        "    var m := arr.Min(); "
        "    return m; "
        "  end GetMin; "
        "  public static method Main(): int return 0; end Main; "
        "end Prog;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "carries a borrow of `arr`"));
}

// REJECT: `e.Dispose()` on a `arr.First()` borrow result — the caller does not
// own it, so disposing it would free a value the receiver still owns (a latent
// double-owner). FAILS WITHOUT THE FIX (a `.Dispose()` on the unrecognized holder
// was silently admitted). // §28.6 / §28.3 v0.194
BEZEL_TEST(bare_type_param_returns_argsource,
           n1876_dispose_of_first_borrow_rejected_spec_28_6) {
    auto r = parseAndCheckOwnership(linqProg(
        "using var arr := new OwningArray<Obj>(); "
        "arr.Add(new Obj()); "
        "var e := arr.First(); "
        "e.Dispose(); "
        "return 0; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        errorSays(r, "cannot call `Dispose()` on a bare-type-parameter call "
                     "result"));
}

// REJECT: `arr.Last()` — element-access borrows the receiver exactly as `First`;
// consuming the receiver while the borrow is live freezes.
BEZEL_TEST(bare_type_param_returns_argsource,
           n1876_last_consume_receiver_freeze_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(linqProg(
        "var arr := new OwningArray<Obj>(); "
        "arr.Add(new Obj()); "
        "var e := arr.Last(); "
        "_ := Sink.Arr(move arr); "
        "var p := e.Poke(); "
        "return p; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "cannot consume `arr`"));
    BEZEL_EXPECT_TRUE(errorSays(r, "borrow held by `e`"));
}

// REJECT: `arr.ElementAt(0)` — positional element-access borrows the receiver.
BEZEL_TEST(bare_type_param_returns_argsource,
           n1876_element_at_consume_receiver_freeze_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(linqProg(
        "var arr := new OwningArray<Obj>(); "
        "arr.Add(new Obj()); "
        "var e := arr.ElementAt(0); "
        "_ := Sink.Arr(move arr); "
        "var p := e.Poke(); "
        "return p; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "cannot consume `arr`"));
    BEZEL_EXPECT_TRUE(errorSays(r, "borrow held by `e`"));
}

// REJECT: `arr.FirstOrNull()` — the `*OrNull` form returns `Obj?`, a nullable
// class borrow of the receiver (stdlib §5's note includes the `*OrNull` forms).
BEZEL_TEST(bare_type_param_returns_argsource,
           n1876_first_or_null_consume_receiver_freeze_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(linqProg(
        "var arr := new OwningArray<Obj>(); "
        "arr.Add(new Obj()); "
        "var e := arr.FirstOrNull(); "
        "_ := Sink.Arr(move arr); "
        "var p := e!.Poke(); "
        "return p; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "cannot consume `arr`"));
    BEZEL_EXPECT_TRUE(errorSays(r, "borrow held by `e`"));
}

// REJECT: `arr.First((o) => true)` — the predicate overload restricts to matching
// elements but still returns a receiver-sourced element borrow (the predicate is
// a function value, runtime-managed, contributing no source of its own).
BEZEL_TEST(bare_type_param_returns_argsource,
           n1876_first_predicate_consume_receiver_freeze_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(linqProg(
        "var arr := new OwningArray<Obj>(); "
        "arr.Add(new Obj()); "
        "var e := arr.First(lambda(o: Obj) => true); "
        "_ := Sink.Arr(move arr); "
        "var p := e.Poke(); "
        "return p; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "cannot consume `arr`"));
    BEZEL_EXPECT_TRUE(errorSays(r, "borrow held by `e`"));
}

// ACCEPT (§28.9 value-substitution gate): on an `Array<int>` the element type is
// a VALUE type, so `arr.First()` returns an `int` COPY, not a borrow — it
// attaches no liveness constraint and consuming the receiver is admitted. This is
// the exact receiver/consume shape the class case above rejects; only the element
// type differs. // §28.9 v0.194
BEZEL_TEST(bare_type_param_returns_argsource,
           n1876_value_typed_element_attaches_nothing_accepted_spec_28_9) {
    auto r = parseAndCheckOwnership(linqProg(
        "var arr := new Array<int>(); "
        "arr.Add(5); "
        "var e := arr.First(); "
        "_ := Sink.Ints(move arr); "  // consuming the receiver is fine — `e` is a copy
        "return e; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
