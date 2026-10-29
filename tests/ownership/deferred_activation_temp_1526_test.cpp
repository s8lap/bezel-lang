// Spec-derived tests for #1526 (§28.3 "Deferred-activation callees", v0.179).
//
// §28.3: "an unbound owned temporary passed as a plain argument to such a
// callee is admissible only when the returned frame is itself fully
// consumed within the same full expression; where the frame outlives the
// expression — bound to a name, returned, stored, or used as a `for`/
// `await for` source (whose desugaring binds it, §29.2) — the program is
// rejected ... The remedy names the source first: `var src := Source(3);
// await for var line in Shout(src) do … end;`." The rule extends to the
// Task family (`Task`, `Task<T>`, `OwningTask<T>`): "an unbound owned
// temporary argument is admissible only when the returned task is consumed
// to settlement within the same full expression — awaited there, or
// blocked on outside a domain ... A task that instead escapes the full
// expression (bound ... returned, stored, or passed to
// `Task.FireAndForget`) is rejected."
//
// Every shape below was hand-verified against `build/bezelc` on a standalone
// probe before being folded into this suite (issue #1526 report).

#include "support/aot_support.hpp"   // runAotChecked (fix #4667)
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

namespace {

// Preamble shared by every test in this file:
//  - Res / MakeRes(): owned Res — the class + static factory used to
//    produce an "unbound owned temporary" argument (§28.3 Producers: a
//    static method's unannotated non-owned return has no receiver source,
//    so the caller treats it as owned — "Factory methods and sourceless
//    returns").
//  - Nums(n)/Doubled(src): IEnumerable<int> — the spec's own synchronous
//    iterator-factory shape (mirrors the issue's t179/p_itertemp.bz probe).
//  - Source(n)/Shout(src): IAsyncEnumerable<int> — the async-iterator
//    sibling, exercising the `await for` desugaring-binds-it escape.
//  - SumAsync(r): Task<int> — the Task-family shape.
//  - Peek(r): int — an ORDINARY (non-deferred-activation) borrowing call,
//    a regression guard that this rule does not over-fire on every unbound
//    temp argument, only deferred-activation callees.
//  - Holder.Items(): IEnumerable<int> — an INSTANCE method (receiver-
//    sourced borrow, not a sourceless static factory), a regression guard
//    that the rule does not flag a receiver-borrowed iterable as an
//    "unbound owned temporary" argument.
std::string wrap(const std::string& body, const std::string& sig) {
    return std::string("module M; "
        "class Res "
        "  public property Value: int; "
        "  public constructor(v: int) "
        "    Value := v; "
        "  end; "
        "end Res; "
        "class Holder "
        "  public method Items(): IEnumerable<int> "
        "    yield return 7; "
        "  end Items; "
        "end Holder; "
        "class Program "
        "  public static method MakeRes(v: int): owned Res "
        "    return new Res(v); "
        "  end MakeRes; "
        "  public static method Peek(r: Res): int => r.Value; "
        "  public static method Nums(n: int): IEnumerable<int> "
        "    var i := 0; "
        "    while i < n do "
        "      yield return i; "
        "      i := i + 1; "
        "    end; "
        "  end Nums; "
        "  public static method Doubled(src: IEnumerable<int>): IEnumerable<int> "
        "    for var x in src do "
        "      yield return x * 2; "
        "    end; "
        "  end Doubled; "
        "  public static async method Source(n: int): IAsyncEnumerable<int> "
        "    var i := 0; "
        "    while i < n do "
        "      yield return i; "
        "      i := i + 1; "
        "    end; "
        "  end Source; "
        "  public static async method Shout(src: IAsyncEnumerable<int>): IAsyncEnumerable<int> "
        "    await for var x in src do "
        "      yield return x * 10; "
        "    end; "
        "  end Shout; "
        "  public static async method SumAsync(r: Res): Task<int> "
        "    return r.Value; "
        "  end SumAsync; "
        "  public ") + sig + " " + body + " end M; "
        "end Program;";
}

}  // namespace

// ---------------------------------------------------------------------------
// Synchronous iterator factory — rejected escapes (§28.3, §29.2, §29.3).
// ---------------------------------------------------------------------------

// The issue's own repro (t179/p_itertemp.bz): `Doubled(Nums(3))` used
// directly as a `for` source. The `for` desugaring binds the frame to an
// implicit `using var` (§29.2), so the unbound temporary `Nums(3)` — a
// sourceless static factory return, §28.3 "Factory methods" — would be
// disposed at the end of the full expression while `Doubled`'s state
// machine still holds a borrow of it. Rejected.
BEZEL_TEST(deferred_activation_temp, iter_for_loop_source_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "for var v in Doubled(Nums(3)) do end; ",
        "static method M()"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// The same escape via an explicit `using var` binding of the frame, rather
// than the `for`-loop desugaring. Rejected.
BEZEL_TEST(deferred_activation_temp, iter_using_var_bound_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "using var t := Doubled(Nums(3)); "
        "for var v in t do end; ",
        "static method M()"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// Synchronous iterator factory — the named-source remedy (§28.3) — clean.
// ---------------------------------------------------------------------------

// The spec's own remedy shape: bind the source to a name first. `src` is a
// named owned binding (not an unbound temporary), so `Doubled(src)`'s frame
// borrowing it is unaffected by this rule — the frame does not outlive
// `src`'s enclosing scope. Clean.
BEZEL_TEST(deferred_activation_temp, iter_named_source_remedy_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "using var src := Nums(3); "
        "for var v in Doubled(src) do end; ",
        "static method M()"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// Async-iterator factory — rejected escape / named-source remedy (§28.3,
// §29.2, §30.7).
// ---------------------------------------------------------------------------

// `await for` is explicitly named by §28.3 as an escape ("used as a `for`/
// `await for` source (whose desugaring binds it, §29.2)"). Rejected.
BEZEL_TEST(deferred_activation_temp, async_iter_await_for_source_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "await for var v in Shout(Source(3)) do end; ",
        "static async method M(): Task"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// The exact remedy the spec text quotes verbatim: `var src := Source(3);
// await for var line in Shout(src) do … end;`. Clean.
BEZEL_TEST(deferred_activation_temp, async_iter_named_source_remedy_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "using var src := Source(3); "
        "await for var v in Shout(src) do end; ",
        "static async method M(): Task"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// Task-family callee — rejected escapes (§28.3, §30.1, §30.3).
// ---------------------------------------------------------------------------

// `using var t := SumAsync(MakeRes(3));` is the spec's own named escape
// example ("bound — `using var t := SumAsync(Nums(3));`"). The task escapes
// the full expression without being consumed to settlement. Rejected.
BEZEL_TEST(deferred_activation_temp, task_using_var_bound_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "using var t := SumAsync(MakeRes(3)); "
        "return 0; ",
        "static async method M(): Task<int>"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Passing the deferred-activation call directly to `Task.FireAndForget` is
// a named escape shape ("passed to `Task.FireAndForget`"). Rejected.
BEZEL_TEST(deferred_activation_temp, task_fire_and_forget_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "Task.FireAndForget(SumAsync(MakeRes(3))); ",
        "static method M()"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// Task-family callee — consumed to settlement within the same full
// expression, and the named-source remedy (§28.3, §30.1) — clean.
// ---------------------------------------------------------------------------

// `var s := await SumAsync(MakeRes(3));` — the task is awaited (consumed to
// settlement) within the same full expression that produces the unbound
// temporary argument. Admitted, exactly as the spec's own example
// (`await SumAsync(Nums(3))`) stays legal. Clean — no over-rejection.
BEZEL_TEST(deferred_activation_temp, task_awaited_in_place_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "var s := await SumAsync(MakeRes(3)); "
        "return s; ",
        "static async method M(): Task<int>"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Bind the source first, then bind and await the task separately — `src` is
// a named owned binding, not an unbound temporary, so the task escaping the
// producing full expression is unaffected by this rule. Clean.
BEZEL_TEST(deferred_activation_temp, task_named_source_remedy_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "using var src := MakeRes(3); "
        "using var t := SumAsync(src); "
        "var s := await t; "
        "return s; ",
        "static async method M(): Task<int>"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// Expression-bodied methods — the `=>` body IS the return value (an escape),
// so it must reject / accept exactly as the block-body `return …;` form does
// (§28.3; the #1547 fix established this block/`=>` parity).
// ---------------------------------------------------------------------------

// `Wrap(): Task<int> => SumAsync(MakeRes(3));` returns the task — an escape —
// with an unbound owned temporary argument. Rejected, exactly as the
// block-body `return SumAsync(MakeRes(3));` form is. // per §28.3
BEZEL_TEST(deferred_activation_temp, exprbody_return_diagnosed) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public property Value: int; "
        "  public constructor(v: int) Value := v; end; "
        "end Res; "
        "class Program "
        "  public static method MakeRes(v: int): owned Res "
        "    return new Res(v); "
        "  end MakeRes; "
        "  public static async method SumAsync(r: Res): Task<int> "
        "    return r.Value; "
        "  end SumAsync; "
        "  public static method Wrap(): Task<int> => SumAsync(MakeRes(3)); "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// The same expression-bodied shape with a NAMED source parameter (`r`, a
// borrow whose source is the caller's frame) rather than an unbound
// temporary — clean, guarding against over-rejection of the `=>` form.
// // per §28.3
BEZEL_TEST(deferred_activation_temp, exprbody_named_source_clean) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public property Value: int; "
        "  public constructor(v: int) Value := v; end; "
        "end Res; "
        "class Program "
        "  public static async method SumAsync(r: Res): Task<int> "
        "    return r.Value; "
        "  end SumAsync; "
        "  public static method Wrap(r: Res): Task<int> => SumAsync(r); "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// Regression guards — the rule must not over-fire.
// ---------------------------------------------------------------------------

// `Peek(r: Res): int` is an ORDINARY borrowing call — its return type (int)
// is not one of the deferred-activation shapes (§29.3/§30.7 iterable, a
// lazy query, or Task-family), so an unbound owned temporary argument is
// governed by the pre-existing base §28.3 "Unbound owned temporaries" rule
// (disposed at the end of the full expression, after the borrowing call
// completes), not this one. Clean.
BEZEL_TEST(deferred_activation_temp, ordinary_non_deferred_callee_not_flagged) {
    auto r = parseAndCheckOwnership(wrap(
        "var v := Peek(MakeRes(3)); ",
        "static method M()"));
    bool sawDeferredActivationError = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("deferred-activation callee") != std::string::npos) {
            sawDeferredActivationError = true;
        }
    }
    BEZEL_EXPECT_FALSE(sawDeferredActivationError);
}

// `Holder.Items()` is an INSTANCE method — its return is a borrow sourced
// from the receiver `h` (§28.3 Producers: "Method call returning `T` (no
// `owned`) ... instance method: source is the receiver"), not a sourceless
// static factory return, so it is not an "unbound owned temporary" at all
// under this rule even though it is a bare, unnamed call syntactically.
// `h` outlives the `for` loop (it is `using var`-bound in the enclosing
// scope), so `Doubled(h.Items())` used directly as a `for` source is clean.
BEZEL_TEST(deferred_activation_temp, instance_method_receiver_borrow_not_flagged) {
    auto r = parseAndCheckOwnership(wrap(
        "using var h := new Holder(); "
        "for var v in Doubled(h.Items()) do end; ",
        "static method M()"));
    bool sawDeferredActivationError = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("deferred-activation callee") != std::string::npos) {
            sawDeferredActivationError = true;
        }
    }
    BEZEL_EXPECT_FALSE(sawDeferredActivationError);
}

// ---------------------------------------------------------------------------
// Owned-sink argument positions are EXEMPT (§28.3: the frame borrows only its
// PLAIN, non-`owned`, reference-typed arguments). These pin the narrowing that
// distinguishes a borrowed argument from an owned-TRANSFERRED one — each shape
// below would be wrongly rejected by a predicate that flagged every unbound
// owned temporary regardless of its target parameter.
// ---------------------------------------------------------------------------

// §29.8 (#945): a fresh owned temporary passed to an iterator factory's `owned`
// parameter is owned-TRANSFERRED into the state machine (which disposes it at
// iterator-Dispose), NOT borrowed — so `for var x in UseCell(new Cell())` is
// clean even though the iterable frame is a `for` source (an escape). The rule
// targets plain reference-typed arguments only. // per §28.3, §29.8
BEZEL_TEST(deferred_activation_temp, owned_param_iterator_source_clean) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell "
        "  public V: int := 7; "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Cell; "
        "class P "
        "  public static method UseCell(owned c: Cell): IEnumerable<int> "
        "    yield return 1; yield return 2; "
        "  end UseCell; "
        "  public static method M() "
        "    for var x in UseCell(new Cell()) do end; "
        "  end M; "
        "end P;");
    bool sawDeferredActivationError = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("deferred-activation callee") != std::string::npos)
            sawDeferredActivationError = true;
    BEZEL_EXPECT_FALSE(sawDeferredActivationError);
}

// §30.14 (#693): `Domain.Spawn`'s extra arguments are owned-TRANSFERRED into the
// spawned domain (the entry disposes them), a builtin owned sink — so a fresh
// owned temporary at a Spawn arg position is clean even though the returned
// task frame is bound to a `using var` (an escape). A builtin callee has no
// resolvable user param list, so the borrow gate leaves it exempt. // per §28.3, §30.14
BEZEL_TEST(deferred_activation_temp, spawn_owned_arg_clean) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Worker "
        "  public static method Run(owned m: Mutex) "
        "    using var s := m; "
        "  end Run; "
        "end Worker; "
        "class Program "
        "  public static async method Main(): Task<int> "
        "    using var t := Domain.Spawn(Worker.Run, new Mutex()); "
        "    await t; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    bool sawDeferredActivationError = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("deferred-activation callee") != std::string::npos)
            sawDeferredActivationError = true;
    BEZEL_EXPECT_FALSE(sawDeferredActivationError);
}

// =====================================================================
// fix #4667 / §28.3 (v0.307): RUNTIME companion to `spawn_owned_arg_clean`
// above. That row only asserts the deferred-activation DIAGNOSTIC does not
// fire; it never ran the program, so the runtime behaviour it exercises —
// an owned `Mutex` argument auto-dropped at the end of a `Domain.Spawn`
// entry via a view binding `var s := m;` (§21.1: a plain alias is a borrow,
// so `m` stays undischarged and the release comes from THIS auto-drop) —
// went unasserted (#4666's census). The binding was written as an unmarked
// `using var s := m;` until spec v0.350, whose §28.10 *Adopting a named
// owned binding* makes that spelling a compile error (it names neither
// adopt nor view); the `var` spelling is the view the rule's remedy offers.
//
// PRE-FIX: `registerUnwindOwnedParam` (LlvmBackend_Setup.cpp) registered
// the §28.3 auto-drop for `ChannelSender`/`Channel`/`Shared` and the
// collection family, but no other vtable-less builtin (Mutex included) —
// `isClassOrInterfaceRef` rejects them too (no user TypeSymbol), so the
// registration silently fell through. The diagnostic this exact call site
// prints ("owned parameter `m` is implicitly disposed at the end of the
// method (§28.3 auto-drop)") promised a release that never ran:
// `bz_mutex_dispose` was hit on NEITHER the direct-call nor the
// `Domain.Spawn`-invoked path (confirmed via a gdb breakpoint on both,
// pre-fix). Loop-amplified 100x (a one-shot leak repro is a vacuous
// oracle — LSan can find the leaked pointer reachable from a live stack
// slot on a single run): pre-fix this leaked the Mutex core every
// iteration. FAILS WITHOUT THE FIX.
// =====================================================================
BEZEL_TEST(deferred_activation_temp, spawn_owned_arg_no_leak_100) {
    auto r = runAotChecked(
        "module M; "
        "class Worker "
        "  public static method Run(owned m: Mutex) "
        "    var s := m; "
        "  end Run; "
        "end Worker; "
        "class Program "
        "  public static async method Main(): Task<int> "
        "    var i := 0; "
        "    while i < 100 do "
        "      using var t := Domain.Spawn(Worker.Run, new Mutex()); "
        "      await t; "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(\"100\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "100\n");
}

// §30.3 (#962): `Task.FromException<T>` takes ownership of its exception
// argument (disposed at task disposal), so a fresh owned exception temporary is
// clean even though the returned `Task<T>` is bound to a `using var` (an
// escape). Task.FromException is a builtin with no resolvable user param list,
// so the borrow gate leaves it exempt — the pre-#1526 admission is preserved.
// // per §28.3, §30.3
BEZEL_TEST(deferred_activation_temp, task_from_exception_owned_arg_clean) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var tf := Task.FromException<int>("
        "        new InvalidOperationException(\"x\")); "
        "    return (if tf.IsFaulted then 1 else 0); "
        "  end Main; "
        "end C;");
    bool sawDeferredActivationError = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("deferred-activation callee") != std::string::npos)
            sawDeferredActivationError = true;
    BEZEL_EXPECT_FALSE(sawDeferredActivationError);
}

// §28.3 mixed positions: a deferred-activation callee with BOTH an `owned` and a
// plain reference parameter. The unbound owned temp at the `owned` position
// (`new Cell()`) is exempt; the unbound temp at the PLAIN position (`Nums(3)`)
// escapes and is rejected — exactly one diagnostic, at the plain argument.
// // per §28.3
BEZEL_TEST(deferred_activation_temp, mixed_owned_and_plain_param_only_plain_diagnosed) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell "
        "  public V: int := 7; "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Cell; "
        "class P "
        "  public static method Nums(n: int): IEnumerable<int> "
        "    yield return n; "
        "  end Nums; "
        "  public static method Mixed(owned c: Cell, src: IEnumerable<int>): IEnumerable<int> "
        "    for var x in src do yield return x; end; "
        "  end Mixed; "
        "  public static method M() "
        "    for var v in Mixed(new Cell(), Nums(3)) do end; "
        "  end M; "
        "end P;");
    int deferredActivationErrors = 0;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("deferred-activation callee") != std::string::npos)
            ++deferredActivationErrors;
    BEZEL_EXPECT_EQ(deferredActivationErrors, 1);
}
