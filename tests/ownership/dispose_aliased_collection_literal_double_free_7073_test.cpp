// Spec-derived regression tests for issue #7073 — an explicit `Dispose()`
// through an ALIAS of a COLLECTION-LITERAL local compiled with NO diagnostic
// and then double-freed at run time.
//
// THE DEFECT (issue #7073, measured by the coordinator on the staging build
// and re-measured on this lane's HEAD):
//
//   var a: Array<int> := [1,2,3];
//   var il: IList<int> := a;
//   il.Dispose();
//
// compiled with NO diagnostic, printed "survived", and then aborted with
// "free(): double free detected in tcache 2" (rc 134). Under generated-code
// AddressSanitizer the report is unambiguous: two `bz_array_free` calls on the
// SAME 64-byte region, one from the explicit dispose and one from the
// compiler-inserted scope-end free, both from `main`.
//
// THE SPEC SENTENCE THAT DECIDES THE OUTCOME — §28.6 "Who may call
// Dispose()" (v0.340):
//
//   "Explicit disposal — `x.Dispose()`, the `x?.Dispose()` / `x!.Dispose()`
//    spellings ... — requires a receiver the disposer **owns** (derives:
//    K3.9): an owned local or `owned` parameter, a `using var`, an unbound
//    owned temporary ..., a `move` result, or an `owned` field ..."
//
//   "`Dispose()` or a direct `DisposeAsync()` call through a **borrow** is a
//    compile error. The borrow receivers: a borrowed local or parameter —
//    including `this` ... The check is structural and decidable — it reads the
//    receiver expression's ownership classification, which §28.3 already
//    computes at every call site."
//
// So the call must be REJECTED — it is a dispose through a borrow — and NOT
// "made to work". The receiver `il` is a borrow by §28.3 "Ownership state of
// locals" (v0.340):
//
//   "`var x := expr;` takes the ownership state of `expr` (see *Producers*
//    below); plain `var` does not change the state. A read of a *named owned
//    binding* on the right-hand side is a **borrow** unless it carries the
//    explicit transfer marker — `var y := move x;`"
//
// and the source `a` IS a named owned binding, because §28.3's *Producers*
// table names the collection-literal row by name:
//
//   "| Collection literal (`[1, 2, 3]`) targeting a reference type (`Array<T>`,
//    `OwningArray<T>`, `Set<T>`, `Dictionary<K, V>`, etc.) | Owned |"
//
// ⇒ `a` owns the array, `il` is a borrow of it, and `il.Dispose()` is the
// §28.6 compile error.
//
// WHY THE COMPILER MISSED IT (measured on this lane). A collection literal is
// CONTEXT-TYPED — §15.4: "A collection literal has no standalone type. It is
// well-typed only in a context that provides the target type" — so sema
// records NO `exprType` for the literal node. `producerState`'s
// collection-literal arm (fix #4338's) therefore never fires, and the
// ownership pass classifies the literal-initialized local as BORROWED. The
// §28.6 alias check reads its source through `disposeAliasSource`, which is
// populated only from a source the pass classifies as OWNED, so the alias of
// a literal-initialized local was invisible to it. The `new`-source twin of
// the same program (`var a: Array<int> := new Array<int>(2);`) IS rejected
// today by fix #4236's arm — the ONLY difference between the two programs is
// the source's initializer form, which is exactly this blind spot.
//
// DIRECTION OF TRAVEL: this fix REJECTS the dispose (it does not change
// codegen's disposal chain), so the double free is unreachable rather than
// merely unlikely — the same shape fix #4236 and fix #4268 took for the
// `new`-source spelling.
//
// ORACLES. Every REJECT row asserts the SPECIFIC diagnostic (the outcome, not
// "did not crash") and runs through `runAotChecked`, so the generated-code
// ASan/LSan tier is armed: on the pre-fix compiler the same program RUNS and
// the sanitizer catches the double free, which is not a compile-time
// rejection, so the row fails on the pre-fix code (fail-without, proved
// mechanically in the commit body). Every ACCEPT-side control row runs under
// `runAotChecked` too, loop-amplified, so a fix that over-rejected, or one
// that left a leak or a second free behind, shows up as a sanitizer report on
// the SAME line.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAotChecked;

namespace {

bool diagContains(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

std::string prog(const std::string& mainBody, const std::string& prelude = "") {
    return "module M; " + prelude +
           "class C public static method Main(): int " + mainBody +
           " end Main; end C;";
}

}  // namespace

// =====================================================================
// REJECT arm — §28.6: a `Dispose()` through a BORROW is a compile error.
// The source is the collection-literal local §28.3's producers table makes
// Owned; the receiver is the alias §28.3 makes a borrow of it.
// =====================================================================

// The issue's own repro, verbatim: an IList-typed alias of a collection
// literal, disposed explicitly.
BEZEL_TEST(ownership_dispose_aliased_collection_literal_7073,
           interface_alias_of_literal_owner_dot_dispose_rejected_28_6) {
    auto r = runAotChecked(prog(
        "var a: Array<int> := [1,2,3]; "
        "var il: IList<int> := a; "
        "il.Dispose(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `il`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§28.6"));
}

// The `?.` spelling — §28.6 names it explicitly ("the `x?.Dispose()` /
// `x!.Dispose()` spellings"), and the issue measured the same double free
// through it.
BEZEL_TEST(ownership_dispose_aliased_collection_literal_7073,
           interface_alias_of_literal_owner_nullcond_dispose_rejected_28_6) {
    auto r = runAotChecked(prog(
        "var a: Array<int> := [1,2,3]; "
        "var il: IList<int> := a; "
        "il?.Dispose(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `il`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§28.6"));
}

// The other array-backed list interface (stdlib §4.1 `IReadOnlyList<T>`) —
// the same §28.6 rule, a different head.
BEZEL_TEST(ownership_dispose_aliased_collection_literal_7073,
           readonlylist_alias_of_literal_owner_dispose_rejected_28_6) {
    auto r = runAotChecked(prog(
        "var a: Array<int> := [1,2,3]; "
        "var il: IReadOnlyList<int> := a; "
        "il.Dispose(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `il`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
}

// The CONCRETE-typed spelling of the same alias: §28.6's rule is about the
// RECEIVER's ownership, not its static type, so the same borrow is the same
// error. (The `new`-source twin of this row is already covered by fix
// #4236's `dispose_aliased_collection_double_free_4236_test.cpp`; this row
// pins the literal-source half.)
BEZEL_TEST(ownership_dispose_aliased_collection_literal_7073,
           concrete_alias_of_literal_owner_dot_dispose_rejected_28_6) {
    auto r = runAotChecked(prog(
        "var a: Array<int> := [1,2,3]; "
        "var b: Array<int> := a; "
        "b.Dispose(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§28.6"));
}

// The `const` spelling — §21.1 tracks a `const` exactly as a plain unmarked
// `var` alias (fix #4236's send-back established this for `disposeAliasSource`
// on the `new`-source shape).
BEZEL_TEST(ownership_dispose_aliased_collection_literal_7073,
           const_alias_of_literal_owner_dot_dispose_rejected_28_6) {
    auto r = runAotChecked(prog(
        "var a: Array<int> := [1,2,3]; "
        "const b: Array<int> := a; "
        "b.Dispose(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
}

// The REASSIGNMENT spelling — the alias is bound after the declaration, so
// the classification is re-derived at the assignment.
BEZEL_TEST(ownership_dispose_aliased_collection_literal_7073,
           reassigned_alias_of_literal_owner_dispose_rejected_28_6) {
    auto r = runAotChecked(prog(
        "var a: Array<int> := [1,2,3]; "
        "var b: Array<int>; "
        "b := a; "
        "b.Dispose(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
}

// The CONST SOURCE — the §21.1 runtime-`const` spelling of the OWNER. The
// const ALIAS above (`const b := a;`) is the other kind of `const` in this
// family; this row is the `const` on the SOURCE side, and it was the hole
// the fix-forward closes.
//
// MEASURED on the pre-fix code (the reviewer's repro, on this lane's HEAD
// before the widening): it compiled with NO diagnostic and then double-freed
// — "AddressSanitizer: attempting double-free … #1 bz_array_free
// src/runtime/bezel_runtime_collections.cpp:741 … 64-byte region" — the same
// defect as the `var`-source row above, the binding KEYWORD alone making the
// difference.
//
// Why the source's binding kind must not matter: §28.3's producers table
// names the collection literal as an Owned production ("Collection literal
// (`[1, 2, 3]`) targeting a reference type (`Array<T>`, `OwningArray<T>`,
// `Set<T>`, `Dictionary<K, V>`, etc.) | Owned"), and §28.3's *Consuming
// positions* makes the unmarked alias binding a borrow of it. §28.6: "`Dispose()`
// or a direct `DisposeAsync()` call through a **borrow** is a compile error."
// §21.1 does not make the `const` spelling a way out: a runtime `const`'s
// initializer must not be an owned production (§28.3), and a collection
// literal targeting a reference type IS one — so this declaration is itself
// a §21.1 error the compiler does not enforce today (#7092's territory). The
// verdict the §28.6 check must return is unchanged by the keyword: `a` owns,
// `il` borrows, `il.Dispose()` is the error.
BEZEL_TEST(ownership_dispose_aliased_collection_literal_7073,
           const_source_alias_dot_dispose_rejected_28_6) {
    auto r = runAotChecked(prog(
        "const a: Array<int> := [1,2,3]; "
        "var il: IList<int> := a; "
        "il.Dispose(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `il`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§28.6"));
}

// The CONCRETE-typed alias of a `const` literal source — the same §28.6 rule
// (the receiver's OWNERSHIP decides, not its static type).
BEZEL_TEST(ownership_dispose_aliased_collection_literal_7073,
           const_source_concrete_alias_dispose_rejected_28_6) {
    auto r = runAotChecked(prog(
        "const a: Array<int> := [1,2,3]; "
        "var b: Array<int> := a; "
        "b.Dispose(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§28.6"));
}

// The `?.` spelling of the const-source alias — §28.6 names it explicitly
// ("the `x?.Dispose()` / `x!.Dispose()` spellings").
BEZEL_TEST(ownership_dispose_aliased_collection_literal_7073,
           const_source_alias_nullcond_dispose_rejected_28_6) {
    auto r = runAotChecked(prog(
        "const a: Array<int> := [1,2,3]; "
        "var il: IList<int> := a; "
        "il?.Dispose(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `il`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
}

// The `!.` spelling of the const-source alias.
BEZEL_TEST(ownership_dispose_aliased_collection_literal_7073,
           const_source_alias_bang_dispose_rejected_28_6) {
    auto r = runAotChecked(prog(
        "const a: Array<int> := [1,2,3]; "
        "var il: IList<int> := a; "
        "il!.Dispose(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `il`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
}

// The alias inside a LAMBDA of a `const` literal source — the closure frame
// does not change the receiver's ownership (§28.3: a captured binding is read
// as a borrow), so the §28.6 verdict stands.
BEZEL_TEST(ownership_dispose_aliased_collection_literal_7073,
           const_source_alias_in_lambda_dispose_rejected_28_6) {
    auto r = runAotChecked(prog(
        "const a: Array<int> := [1,2,3]; "
        "var f := lambda() => do "
        "  var il: IList<int> := a; "
        "  il.Dispose(); "
        "end; "
        "f(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `il`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§28.6"));
}

// The `!.Dispose()` spelling of the plain `var`-source alias — §28.6 names
// it explicitly ("the `x?.Dispose()` / `x!.Dispose()` spellings"). Pinned
// here so the `!.` arm of the §28.6 check cannot silently stop matching the
// literal-source family.
BEZEL_TEST(ownership_dispose_aliased_collection_literal_7073,
           bang_alias_of_literal_owner_dispose_rejected_28_6) {
    auto r = runAotChecked(prog(
        "var a: Array<int> := [1,2,3]; "
        "var il: IList<int> := a; "
        "il!.Dispose(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `il`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
}

// The other §28.3 producers-table heads: `Dictionary<K, V>` (stdlib §4.8).
BEZEL_TEST(ownership_dispose_aliased_collection_literal_7073,
           dictionary_alias_of_literal_owner_dispose_rejected_28_6) {
    auto r = runAotChecked(prog(
        "var d: Dictionary<string, int> := [\"a\": 1]; "
        "var il: ICollection<DictionaryEntry<string, int>> := d; "
        "il.Dispose(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `il`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `d`"));
}

// `Set<T>` (stdlib §4.10).
BEZEL_TEST(ownership_dispose_aliased_collection_literal_7073,
           set_alias_of_literal_owner_dispose_rejected_28_6) {
    auto r = runAotChecked(prog(
        "var s: Set<int> := [1,2,3]; "
        "var il: ICollection<int> := s; "
        "il.Dispose(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `il`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `s`"));
}

// `Stack<T>` (stdlib §4.11) — conforms only to `IReadOnlyCollection<T>`.
BEZEL_TEST(ownership_dispose_aliased_collection_literal_7073,
           stack_alias_of_literal_owner_dispose_rejected_28_6) {
    auto r = runAotChecked(prog(
        "var st: Stack<int> := [1,2,3]; "
        "var il: IReadOnlyCollection<int> := st; "
        "il.Dispose(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `il`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `st`"));
}

// `LinkedList<T>` (stdlib §4.6).
BEZEL_TEST(ownership_dispose_aliased_collection_literal_7073,
           linkedlist_alias_of_literal_owner_dispose_rejected_28_6) {
    auto r = runAotChecked(prog(
        "var ll: LinkedList<int> := [1,2,3]; "
        "var il: ICollection<int> := ll; "
        "il.Dispose(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `il`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `ll`"));
}

// =====================================================================
// ACCEPT-side controls (LANE_RULES §15) — programs that MUST keep compiling
// and running. A false-reject here would be worse than the defect.
//
// (z1) A LEGAL runtime `const` whose initializer IS a collection literal, but
//      whose target is an INLINE-ARRAY type. §28.3's producers table says
//      that row is "Not applicable — produces a value", so it is NOT an owned
//      production and §21.1 ADMITS it: "the initializer of a runtime `const`
//      must not be an **owned production**". This row is the guard that the
//      widening did not turn a legal runtime `const` into a compile error:
//      the same collection-literal initializer, a different (value) target.
// =====================================================================
BEZEL_TEST(ownership_dispose_aliased_collection_literal_7073,
           const_inline_array_literal_still_compiles_28_3) {
    auto r = runAotChecked(prog(
        "var i := 0; "
        "while i < 200 do "
        "  const xs: [3]int := [1,2,3]; "
        "  i := i + xs[0] - 1; "
        "  i := i + 1; "
        "end; "
        "return 0;"));
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// (z2) The `const` literal SOURCE used on its own — no alias, no dispose.
//      This is the over-rejection guard for the widening: the widening only
//      feeds the §28.6 borrow-receiver check, so a `const` literal owner
//      that is never aliased must still compile and must still free its
//      array exactly once (at its scope end). It is loop-amplified, so a
//      second free or a leak is a sanitizer report on the same line.
//
//      NOTE (deferred, #7092): §21.1 itself says a runtime `const`'s
//      initializer must not be an owned production, and a collection literal
//      targeting a reference type IS one — so this declaration is, per the
//      spec, a compile error the compiler does not enforce today. This row
//      pins the CURRENT verdict (accepted) so that this lane's widening is
//      visibly not the thing that changes it; a future §21.1 enforcement
//      must update this row.
BEZEL_TEST(ownership_dispose_aliased_collection_literal_7073,
           const_literal_owner_never_aliased_still_runs_28_3) {
    auto r = runAotChecked(prog(
        "var i := 0; "
        "while i < 200 do "
        "  const a: Array<int> := [1,2,3]; "
        "  var n := a.Count; "
        "  i := i + n - 3; "
        "  i := i + 1; "
        "end; "
        "return 0;"));
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// (a) The OWNER's own dispose: `a` holds the literal, so `a.Dispose()` is
//     §28.6's "an owned local" receiver — legal, and it must free exactly
//     once (loop-amplified, so a second free or a leak is a sanitizer
//     report on the same line).
// =====================================================================
BEZEL_TEST(ownership_dispose_aliased_collection_literal_7073,
           literal_owner_own_dispose_frees_exactly_once_28_6) {
    auto r = runAotChecked(prog(
        "var i := 0; "
        "while i < 200 do "
        "  var a: Array<int> := [1,2,3]; "
        "  a.Dispose(); "
        "  i := i + 1; "
        "end; "
        "return 0;"));
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// (b) The OWNED-PRODUCER spelling of the interface head: `new Array<int>(2)`
//     is a fresh owned production, so the interface-typed local OWNS it and
//     `il.Dispose()` is §28.6's "an owned local" receiver — legal.
BEZEL_TEST(ownership_dispose_aliased_collection_literal_7073,
           owned_producer_interface_dispose_still_runs_28_6) {
    auto r = runAotChecked(prog(
        "var i := 0; "
        "while i < 200 do "
        "  var il: IList<int> := new Array<int>(2); "
        "  il.Dispose(); "
        "  i := i + 1; "
        "end; "
        "return 0;"));
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// (c) The TRANSFER spelling §28.3 names: `var il: IList<int> := move a;`
//     takes `a`'s obligation outright, so `il` is the owner and its
//     explicit dispose is legal — and frees exactly once.
BEZEL_TEST(ownership_dispose_aliased_collection_literal_7073,
           moved_literal_owner_interface_dispose_frees_exactly_once_28_8_1) {
    auto r = runAotChecked(prog(
        "var i := 0; "
        "while i < 200 do "
        "  var a: Array<int> := [1,2,3]; "
        "  var il: IList<int> := move a; "
        "  il.Dispose(); "
        "  i := i + 1; "
        "end; "
        "return 0;"));
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// (d) The alias DECLARED BUT NEVER DISPOSED must still compile (the §28.6
//     check fires only AT a dispose call site) and must still free the
//     collection exactly once — at the OWNER's scope end, since the alias
//     carries no obligation of its own.
BEZEL_TEST(ownership_dispose_aliased_collection_literal_7073,
           interface_alias_never_disposed_frees_exactly_once_28_3) {
    auto r = runAotChecked(prog(
        "var i := 0; "
        "while i < 200 do "
        "  var a: Array<int> := [1,2,3]; "
        "  var il: IList<int> := a; "
        "  var n := il.Count; "
        "  i := i + n - n; "
        "  i := i + 1; "
        "end; "
        "return 0;"));
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 0);
}
