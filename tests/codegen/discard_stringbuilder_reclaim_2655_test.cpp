// Codegen — issue #2655, shape 2 (§17.13 the discard `_`, §28.3 unbound owned
// temporaries, §28.9 trivial-dispose classes; spec v0.237): `_ := new
// StringBuilder();` — the spec's OWN §17.13 example — leaked the builder's
// entire parts array.
//
// §17.13's trivial-dispose bullet reads: "discard is permitted. The class has a
// no-op `Dispose()` by definition, so abandoning the value cannot leak a
// destructor invocation. Memory is reclaimed by an implementation-defined
// mechanism (see §28's preamble)." No such mechanism existed for this shape:
// the memory simply leaked.
//
// Root cause (read on main ce1c6a8 before the fix): the ownership pass's
// end-of-statement reclaim gate for a discarded owned PRODUCER
// (src/ownership/Ownership.cpp:2503-2516) requires the produced type to be a
// USER class/interface — `rt->declaration->decl != nullptr` — with the reason
// stated verbatim in its own comment: "builtin handles (Task, Subscription,
// Shared, Stopwatch, …) resolve to a declaration-less / decl-null TypeSymbol
// and need their own free paths (deferred)". StringBuilder is exactly such a
// decl-null builtin, and the sibling gate #1909 added
// (`disposeOwnedCollectionValue`) covers only the OWNING-collection family. So
// no branch fired at all and the builder's `bz_array_alloc`'d parts array — the
// value a StringBuilder *is* (#1185) — was never freed. The asymmetry was the
// tell: `using var sb := new StringBuilder();` has been freed since #1185
// through a proven path; only the discarded temporary had none.
//
// Fix: `ast::DiscardStmt::disposeOwnedBuilderValue`, set by the ownership pass
// under #1009's unchanged `producerState == Owned` gate narrowed to the
// StringBuilder type-head, consumed by a codegen arm that reuses the #1185
// `using var` wiring VERBATIM — `Disposal{slot}` with `collection = true` and
// `collectionElemDisposer = stringElementReleaseThunk()`, i.e. bz_array_free
// with the per-part `bz_string_release`. Deliberately scoped to StringBuilder
// alone: it is the one decl-null handle with an already-proven free path, and
// the rest of that deferral stands (`Task` may not even be discarded, §17.13).
//
// SOLE-OWNER PROOF (why this cannot double-free): the gate is `producerState ==
// Owned`, which admits only a FRESH production. The one shape that could
// collide — the fluent `_ := sb.Append("x");`, whose result is the receiver —
// is a plain non-`owned` instance-method return, classified BORROWED by K1.5
// ("an instance method's plain class-typed return is a borrow whose source is
// the receiver"), so it is left unmarked and the receiver's own `using var`
// free stays the only one. A *named* builder binding (`_ := sb;`) takes the
// earlier `discardOutOwnedVar` branch, which this arm sits after and does not
// alter. Both are pinned below.
//
// EXACTLY-ONCE ORACLE: a trivial-dispose type has no destructor to count from
// (§28.9), so the sanitizer is the counter — under `runAotChecked` a missing
// free is an LSan leak and a second free an ASan double-free, and both flip
// `r.ran` to false -> the -999 sentinel; the asserted value pins that nothing
// live was freed underneath.
//
// fail-without/pass-with (measured on ce1c6a8 with `--sanitize=address`):
// `_ := new StringBuilder();` leaked 560 B in 2 allocations (the 48 B
// bz_array_t header + its 512 B data buffer). With the fix, clean.
// per §17.13, §28.3, §28.9 (issue #2655, spec v0.237)

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
int runChecked(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}
int runValueOnly(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── The spec's own §17.13 example, verbatim in shape.
// Without the fix: 560 B leaked in 2 allocations (measured on ce1c6a8).
// Value: 0. // per §17.13, §28.9 (issue #2655)
BEZEL_TEST(discard_stringbuilder_reclaim_2655, discard_new_stringbuilder_spec_17_13) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    _ := new StringBuilder(); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        0);
}

// ── CONSTRUCTOR-FORM axis, all three stdlib §3.1 overloads in one program: the
// no-arg form, the `initial: string` form (whose part the builder RETAINS — the
// free must release that +1 too, or the string leaks on top of the array), and
// the `capacity: int` form (#1396). A single shared disposer thunk must handle
// all three. Value: 0. // per stdlib §3.1, §17.13 (issue #2655)
BEZEL_TEST(discard_stringbuilder_reclaim_2655, discard_all_ctor_forms_stdlib_3_1) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    _ := new StringBuilder(); "
        "    _ := new StringBuilder(\"hello\"); "
        "    _ := new StringBuilder(64); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        0);
}

// ── STRING-PARTS axis with a +1 PRODUCER part: the discarded builder holds a
// concatenation result (a freshly minted +1 string, #1185's retaining-Append
// contract). The element release thunk must drop that +1 as well as free the
// array, or the string outlives its only holder. A discarded builder built up
// across several statements is the realistic shape. Value: 0.
// per stdlib §3.1, §17.13, §28.9 (issue #2655)
BEZEL_TEST(discard_stringbuilder_reclaim_2655, discard_builder_with_produced_parts_stdlib_3_1) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    _ := new StringBuilder(\"a\" + \"b\"); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        0);
}

// ── LOUD form: the discard inside a loop, so a missing free is 8 leaked
// builders, not 1. The free must fire per ITERATION. Value: 8.
// per §17.13, §28.3 (issue #2655)
BEZEL_TEST(discard_stringbuilder_reclaim_2655, discard_builder_in_loop_frees_per_iteration) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var n := 0; "
        "    for var i in 1..8 do "   // §17.9: `a..b` is INCLUSIVE — 8 iterations
        "      _ := new StringBuilder(); "
        "      n := n + 1; "
        "    end; "
        "    return n; "
        "  end Main; "
        "end C;"),
        8);
}

// ── DOUBLE-FREE PRE-EMPT (the one that matters). The FLUENT discard
// `_ := sb.Append("ab");` — `Append` returns the receiver, a BORROW (K1.5), and
// the receiver's `using var` already owns it. If the new gate fired here the
// builder would be freed at the discard AND at scope end: a double free, with
// every later Append a UAF. Asserted BOTH ways — ASan/LSan-clean, and correct
// on the plain uninstrumented path too (#2629: ASan can render a wild free
// benign that the plain build crashes on).
// Value: "ab" + "cd\n" = 5. // per §17.13, §21.9 (issue #2655 self-pass)
BEZEL_TEST(discard_stringbuilder_reclaim_2655, fluent_discard_not_freed_no_double_free) {
    const std::string prog =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var sb := new StringBuilder(); "
        "    _ := sb.Append(\"ab\"); "
        "    _ := sb.AppendLine(\"cd\"); "
        "    return sb.ToString().Length; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 5);
    BEZEL_EXPECT_EQ(runValueOnly(prog), 5);
}

// ── REGRESSION GUARD for the untouched neighbour: a `using var` builder that
// is used normally. Its #1185 scope-end free must remain the ONLY one — this
// program contains no discard at all, so any change in its behaviour would mean
// the new arm fired somewhere it must not. Value: 4.
// per stdlib §3.1, §28.10 (issue #2655 self-pass)
BEZEL_TEST(discard_stringbuilder_reclaim_2655, using_var_builder_unchanged_stdlib_3_1) {
    const std::string prog =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var sb := new StringBuilder(); "
        "    sb.Append(\"ab\"); "
        "    sb.Append(\"cd\"); "
        "    return sb.ToString().Length; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 4);
    BEZEL_EXPECT_EQ(runValueOnly(prog), 4);
}
