// Spec-derived tests for owned-sink argument checking (issue #395).
//
// §28.8 (ownership transfer): "When a parameter is `owned`, the caller
// relinquishes ownership and may not use the argument after the call."
// §28.3 producers table: reading a *borrowed* local (or a borrowed
// parameter / `this`) in an owned-consuming position is not a legal
// ownership transfer — only an *owned* value may flow into an `owned`
// parameter. The owned value forms are: a `move` of an owned local
// (§28.3 move-semantics), a `new` expression (§28.8 "A `new` expression
// produces an owned value"), or a call returning `owned T` (§28.3
// producers, "Method call returning `owned T` | Owned"). The owned-field
// `move this.f` and owned-slot `move coll[i]` forms (§28.8.1) are also
// owned.
//
// The under-enforcement this file pins down: the ownership pass admitted
// a *borrow* where an `owned` argument was required (e.g.
// `OwningArray<T>.Add(owned item)`, the owning-collection `set(owned)`
// indexer). A borrow handed to an owned sink would be disposed by the
// sink AND by its real owner — a double-dispose. The POSITIVE cases below
// must stay ACCEPTED (no over-rejection of valid `move`/`new`/owned-call
// transfers); the NEGATIVE cases must now be REJECTED.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

// A `Sink` modeling the owning-collection surface from stdlib §4.5 /
// §4.9: `Add(owned item: T)` and a `set(owned)` indexer, plus a factory
// `MakeOwned(): owned T` (an owned-returning call, the third legal owned
// argument form). The owned `item`/`value` obligations are discharged
// inside the bodies (`using var hold := move …`) so the Sink itself
// surfaces no unrelated diagnostics.
std::string mod(const std::string& body) {
    return std::string("module M; "
        "class T end T; "
        "class Sink "
        "  private owned _slot: T := new T(); "
        "  public method Add(owned item: T) "
        "    using var hold := move item; "
        "  end Add; "
        "  public method MakeOwned(): owned T "
        "    return new T(); "
        "  end MakeOwned; "
        "  public property [i: int]: T "
        "    get => this._slot; "
        "    set(owned) => do "
        "      using var hold := move value; "
        "    end; "
        "  end; "
        "end Sink; ") + body;
}

} // namespace

// ---- POSITIVE cases: legal owned arguments, must stay ACCEPTED ----------

// §28.3 move-semantics: `move x` of an owned local transfers ownership
// into the `owned` parameter. `x` is left moved-from (discharged).
BEZEL_TEST(owned_sink, add_move_owned_local_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class C "
        "  public method M(s: Sink) "
        "    var x := new T(); "
        "    s.Add(move x); "
        "  end M; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.3 (v0.169, #1348): a NAMED OWNED BINDING (owned local) in an owned-
// consuming position requires the call-site `move` marker — `s.Add(move x)`
// transfers ownership (x → moved-from) and the marker announces it. (The bare
// `s.Add(x)` form is now rejected; see marker_transfer_test.cpp.)
// per §A.4 / proposal 0090 (v0.246): call-site transfer uses `move`, not `owned`.
BEZEL_TEST(owned_sink, add_marked_owned_local_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class C "
        "  public method M(s: Sink) "
        "    var x := new T(); "
        "    s.Add(move x); "
        "  end M; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.8: a `new` expression produces an owned value; passing it directly
// to an `owned` parameter transfers that fresh ownership in.
BEZEL_TEST(owned_sink, add_new_expr_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class C "
        "  public method M(s: Sink) "
        "    s.Add(new T()); "
        "  end M; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3 producers: a call returning `owned T` produces an owned value;
// passing the result to an `owned` parameter is a legal transfer.
BEZEL_TEST(owned_sink, add_owned_returning_call_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class C "
        "  public method M(s: Sink) "
        "    s.Add(s.MakeOwned()); "
        "  end M; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.8.1 / stdlib §4.5: the owning-collection `set(owned)` indexer with a
// `move`-of-owned-local RHS is a legal transfer.
BEZEL_TEST(owned_sink, indexer_set_move_local_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class C "
        "  public method M(s: Sink) "
        "    var x := new T(); "
        "    s[0] := move x; "
        "  end M; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.8 / stdlib §4.5: the `set(owned)` indexer with a `new`-expr RHS.
BEZEL_TEST(owned_sink, indexer_set_new_expr_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class C "
        "  public method M(s: Sink) "
        "    s[0] := new T(); "
        "  end M; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.8: an `owned` parameter of the caller is itself an owned value; a
// `move` of it into another `owned` sink is a legal forward-transfer.
BEZEL_TEST(owned_sink, add_move_owned_param_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class C "
        "  public method M(s: Sink, owned p: T) "
        "    s.Add(move p); "
        "  end M; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A plain (non-owned) parameter passed to a non-owned parameter is an
// ordinary borrow-in/borrow-through — unaffected by the owned-sink rule.
BEZEL_TEST(owned_sink, borrow_to_plain_param_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class Inspector public method Peek(x: T) _ := 1; end Peek; end Inspector; "
        "class C "
        "  public method M(i: Inspector, b: T) "
        "    i.Peek(b); "
        "  end M; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---- NEGATIVE cases: a borrow into an owned sink, must be REJECTED -------

// §28.8: a borrowed parameter is not an owned value; passing it to
// `Add(owned item)` would let the sink dispose a value it does not own
// (double-dispose with the real owner). REJECTED.
BEZEL_TEST(owned_sink, add_borrowed_param_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class C "
        "  public method M(s: Sink, b: T) "
        "    s.Add(b); "
        "  end M; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a borrow cannot be passed where an `owned` value is required"));
}

// §28.8: the same with an explicit `owned` call-site modifier on a borrow
// — still a borrow, still REJECTED. Now at parse time (per §A.4 / proposal 0090).
BEZEL_TEST(owned_sink, add_owned_modifier_on_borrowed_param_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class C "
        "  public method M(s: Sink, b: T) "
        "    s.Add(owned b); "
        "  end M; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "invalid use of 'owned' at argument position"));
}

// §28.8 / §28.3: `move` of a borrowed parameter is not a legal owned
// transfer (only owned locals/params have ownership to move). REJECTED.
BEZEL_TEST(owned_sink, add_move_borrowed_param_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class C "
        "  public method M(s: Sink, b: T) "
        "    s.Add(move b); "
        "  end M; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a borrow cannot be passed where an `owned` value is required"));
}

// §28.8: a borrowed *local* (bound to a borrow source that the caller does
// not own) handed to `Add(owned item)` is a borrow into an owned sink.
// Here `b` borrows the receiver's slot (the indexer getter returns a
// borrow), so it is in the Borrowed state — REJECTED.
BEZEL_TEST(owned_sink, add_borrowed_local_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class C "
        "  public method M(s: Sink) "
        "    var b := s[0]; "
        "    s.Add(b); "
        "  end M; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a borrow cannot be passed where an `owned` value is required"));
}

// §28.8 / stdlib §4.5: the owning-collection `set(owned)` indexer with a
// borrowed-parameter RHS — a borrow into an owned slot. The slot would
// dispose a value it does not own. REJECTED. (Spec §28.11: "Indexer set
// `coll[i] := value` requires `value` to be owned.")
BEZEL_TEST(owned_sink, indexer_set_borrowed_param_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class C "
        "  public method M(s: Sink, b: T) "
        "    s[0] := b; "
        "  end M; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a borrow cannot be passed where an `owned` value is required"));
}

// §28.8: passing a borrowed parameter as the `owned` argument of a
// *constructor* is the same hazard. REJECTED.
BEZEL_TEST(owned_sink, new_ctor_borrowed_param_rejected) {
    auto r = parseAndCheckOwnership(std::string("module M; class T end T; "
        "class Box "
        "  public constructor(owned x: T) "
        "    using var hold := move x; "
        "  end constructor; "
        "end Box; "
        "class C "
        "  public method M(b: T) "
        "    using var box := new Box(b); "
        "  end M; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a borrow cannot be passed where an `owned` value is required"));
}

// ---- Built-in owning-collection sinks (stdlib §4.5) ----------------------
//
// The built-in `OwningArray<T>` has no AST declaration the analysis can read,
// so its owned-taking methods are recognized by receiver-type + method name.
// These pin the issue's headline case (`OwningArray<T>.Add(owned item)`).

namespace {
// Wrap a method body in a class with a disposable element type `R`.
std::string bi(const std::string& method) {
    return std::string("module M; class R end R; class C ") + method + " end C;";
}
} // namespace

// stdlib §4.5: `OwningArray<T>.Add` takes ownership; `move x` of an owned local
// is a legal transfer. ACCEPTED.
BEZEL_TEST(owned_sink, builtin_array_add_move_local_accepted) {
    auto r = parseAndCheckOwnership(bi(
        "public method M(a: OwningArray<R>) "
        "  var x := new R(); "
        "  a.Add(move x); "
        "end M;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// stdlib §4.5: `OwningArray<T>.Add(new R())` transfers fresh ownership in.
// ACCEPTED.
BEZEL_TEST(owned_sink, builtin_array_add_new_accepted) {
    auto r = parseAndCheckOwnership(bi(
        "public method M(a: OwningArray<R>) "
        "  a.Add(new R()); "
        "end M;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// stdlib §4.5: a borrowed parameter handed to `OwningArray<T>.Add(owned item)`
// is a borrow into an owned sink — double-dispose hazard. REJECTED (§28.8).
BEZEL_TEST(owned_sink, builtin_array_add_borrow_rejected) {
    auto r = parseAndCheckOwnership(bi(
        "public method M(a: OwningArray<R>, b: R) "
        "  a.Add(b); "
        "end M;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a borrow cannot be passed where an `owned` value is required"));
}

// stdlib §4.9: `OwningDictionary<K, V>.Add(key, owned value)` — the owned
// argument is the *second* one. A borrow there is REJECTED; the key is an
// ordinary value and is unaffected.
BEZEL_TEST(owned_sink, builtin_dict_add_borrow_value_rejected) {
    auto r = parseAndCheckOwnership(bi(
        "public method M(d: OwningDictionary<string, R>, b: R) "
        "  d.Add(\"k\", b); "
        "end M;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a borrow cannot be passed where an `owned` value is required"));
}

// stdlib §6.7 / §30.10 (#1121): Task.WhenAllOwned<T>(owned tasks:
// OwningArray<OwningTask<T>>) is an owned sink — passing a bare owned local
// `tasks` is an implicit §28.8 transfer that discharges its disposal
// obligation (the documented `await Task.WhenAllOwned(tasks)` form). Before the
// fix the intrinsic had no owned-sink entry, so `tasks` was reported as "dropped
// without disposal" — a spurious reject of canonical code.
BEZEL_TEST(owned_sink, task_whenallowned_consumes_owned_array_1121) {
    auto r = parseAndCheckOwnership(
        "module M; import System.Collections.Generic; "
        "class Res public n: int; public constructor(v: int) n := v; end constructor; end Res; "
        "class C "
        "  public static async method Make(v: int): OwningTask<Res> return new Res(v); end Make; "
        "  public static async method LoadAll(): OwningTask<OwningArray<Res>> "
        "    var tasks := new OwningArray<OwningTask<Res>>(); "
        "    tasks.Add(Make(3)); "
        "    using var wt := Task.WhenAllOwned<Res>(move tasks); "
        "    return await wt; "
        "  end LoadAll; "
        "  public static method Main(): int => 0; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Parity guard: the explicit `move tasks` form (the spec-canonical owned
// transfer) compiles identically — the implicit form above must match it.
BEZEL_TEST(owned_sink, task_whenallowned_explicit_move_accepted_1121) {
    auto r = parseAndCheckOwnership(
        "module M; import System.Collections.Generic; "
        "class Res public n: int; public constructor(v: int) n := v; end constructor; end Res; "
        "class C "
        "  public static async method Make(v: int): OwningTask<Res> return new Res(v); end Make; "
        "  public static async method LoadAll(): OwningTask<OwningArray<Res>> "
        "    var tasks := new OwningArray<OwningTask<Res>>(); "
        "    tasks.Add(Make(3)); "
        "    using var wt := Task.WhenAllOwned<Res>(move tasks); "
        "    return await wt; "
        "  end LoadAll; "
        "  public static method Main(): int => 0; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
