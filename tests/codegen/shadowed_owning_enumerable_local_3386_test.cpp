// Codegen regression #3386 — §2.3 (Always-available names) / §29.2 (Desugaring
// of `for`), v0.282.
//
// THE DEFECT. `Emitter::lowerLocalDecl`'s enumerable-local coercion gate
// (LlvmBackend_Setup.cpp) read
//
//     if (declTR && declTR->kind == Named &&
//         !declTR->declaration &&
//         declTR->astNode && declTR->astNode->name &&
//         !declTR->astNode->name->parts.empty() &&
//         declTR->headName() == "IEnumerable" ||
//         declTR->headName() == "IOwningEnumerable") {
//
// `&&` binds tighter than `||`, so it parsed as
//
//     (declTR && … && head == "IEnumerable") || (head == "IOwningEnumerable")
//
// and the second disjunct was evaluated with NONE of the preceding guards —
// including the `!declTR->declaration` BUILTIN-IDENTITY check, whose entire job
// is to keep a USER-DECLARED type of the same name out of the builtin
// enumerable wrap path. (clang flagged it on every build:
// `-Wlogical-op-parentheses`, "'&&' within '||'".) Introduced by 45fa15a
// (#1838), which appended the `IOwningEnumerable` head without parenthesising.
//
// WHY THE PROGRAM BELOW IS LEGAL. §2.3 lists `IOwningEnumerable<T>` in the
// "Iteration and collection interfaces" row with Shadowable = **Yes**, and
// states "every other always-available name may be shadowed by local
// declarations under the normal name-resolution rules". So a module may declare
// its own `interface IOwningEnumerable<T>`; a local typed at that shadow is an
// ordinary user-interface local with no iteration semantics whatsoever, and
// §29.2's enumerable coercion must not touch it.
//
// WHAT WENT WRONG. Because the mis-parse skipped `!declTR->declaration`, the
// local `e: IOwningEnumerable<int>` (declTR->declaration = the USER interface)
// entered the wrap block. Its initializer is a user class carrying a
// declaration, so the block's last arm called `wrapUserClassAsIEnumerable`,
// which found `Src.GetEnumerator()`, resolved `DummyEnum` as the enumerator
// class, and stored a freshly built §29.11 enumerator ADAPTER struct in `e`'s
// slot in place of the `Src` instance. The subsequent `e.Tag()` then dispatched
// a user-interface call through the adapter — MEASURED: SIGSEGV (exit 139) on
// e9bd762, and the same crash under generated-code ASan.
//
// FAIL-WITHOUT: the program crashes, so `runAotChecked(...).ran` is false and
// the helper reports -999. PASS-WITH: 42 (`Src.Tag()`).
//
// `DummyEnum` exists because `wrapUserClassAsIEnumerable` declines (returns the
// value unchanged) when it cannot resolve a MoveNext+Current enumerator class —
// a decline would make the mis-parse observationally identical to the fix. It
// is never instantiated at runtime: `GetEnumerator` is never called on this
// path, which is also why the ASan/LSan run is clean.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
int run(const std::string& s) {
    auto r = runAotChecked(s);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ---------------------------------------------------------------------------
// #3386 / §2.3 + §29.2: a local declared at a USER-DECLARED interface that
// shadows the auto-imported `IOwningEnumerable<T>` must NOT be routed through
// the builtin enumerable coercion — it is not the builtin interface.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_shadowed_owning_enumerable_3386,
           user_shadow_of_iowningenumerable_local_is_not_wrapped) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IOwningEnumerable<T> "
        "  method Tag(): int; "
        "end IOwningEnumerable; "
        "class DummyEnum "
        "  public method MoveNext(): bool => false; "
        "  public method Current(): int => 0; "
        "end DummyEnum; "
        "class Src: IOwningEnumerable<int> "
        "  public method Tag(): int => 42; "
        "  public method GetEnumerator(): owned DummyEnum => new DummyEnum(); "
        "end Src; "
        "class M2 public static method Main(): int "
        "  using var e: IOwningEnumerable<int> := new Src(); "
        "  return e.Tag(); "
        "end Main; end M2;"), 42);
}

// ---------------------------------------------------------------------------
// The CONTROL that pins which grouping is intended. The identical program with
// the shadow named `IEnumerable` already behaved correctly BEFORE the fix,
// because that head sits in the FIRST disjunct and so kept every guard —
// `!declTR->declaration` excluded the shadow exactly as designed. The two heads
// must not disagree: the parenthesised grouping is the one that makes them
// agree, and this arm fails if a future edit ever moves `IEnumerable` out from
// behind the guards.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_shadowed_owning_enumerable_3386,
           user_shadow_of_ienumerable_local_is_not_wrapped) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IEnumerable<T> "
        "  method Tag(): int; "
        "end IEnumerable; "
        "class DummyEnum "
        "  public method MoveNext(): bool => false; "
        "  public method Current(): int => 0; "
        "end DummyEnum; "
        "class Src: IEnumerable<int> "
        "  public method Tag(): int => 43; "
        "  public method GetEnumerator(): owned DummyEnum => new DummyEnum(); "
        "end Src; "
        "class M2 public static method Main(): int "
        "  using var e: IEnumerable<int> := new Src(); "
        "  return e.Tag(); "
        "end Main; end M2;"), 43);
}

// ---------------------------------------------------------------------------
// The ACCEPT-CONTROL: the builtin `IOwningEnumerable<T>` head still takes the
// coercion. A local declared at the BUILTIN interface and initialized from a
// `yield return` iterator must still drive the vtable protocol
// (GetOwningEnumerator()/TakeCurrent()) — i.e. the fix narrows the gate to
// exactly the builtin-identity case rather than closing it. Returns 30.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_shadowed_owning_enumerable_3386,
           builtin_owning_enumerable_local_still_coerced) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class R "
        "  public id: int; "
        "  public constructor(x: int) id := x; end constructor; "
        "end R; "
        "class C "
        "  public static method Gen(): IOwningEnumerable<R> "
        "    yield return new R(10); "
        "    yield return new R(20); "
        "  end Gen; "
        "end C; "
        "class M2 public static method Main(): int "
        "  var total: int := 0; "
        "  var e: IOwningEnumerable<R> := C.Gen(); "
        "  for var x in e do "
        "    using var y := owned x; "
        "    total := total + y.id; "
        "  end; "
        "  return total; "
        "end Main; end M2;"), 30);
}
