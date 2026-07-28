// Codegen regression #1838 — §29.2 IOwningEnumerable local/param sources.
//
// HISTORY (2026-07-29): this file previously held two tests, BOTH of which
// asserted programs the language does not admit, and both had been failing on
// origin/main since 45fa15a landed:
//
//   * `local_source_enumerates` built `new OwningArray<int>()`. The owning
//     collections constrain `where T: class` (stdlib §4.2 / §14.4, #1327) —
//     "a primitive value type has no disposal obligation and cannot be owned in
//     an owning collection" — so the program is rejected, `runAotChecked`
//     reports ran=false, and the helper's -999 never equalled the expected 3.
//     It also called `.Add` on an `IOwningEnumerable<T>`-typed variable, which
//     that interface does not declare.
//
//   * `array_widening_disposes_cleanly` bound `using var e:
//     IOwningEnumerable<int> := arr` from an `Array<int>`. `Array<T>` is
//     declared `Array<T>: IList<T>` (stdlib §4.4) and does NOT implement
//     `IOwningEnumerable<T>`, so the widening is not an implicit conversion —
//     the compiler correctly rejects it with "the initializer for `e` has a
//     type that is not implicitly convertible to the target type (§5.1)".
//
// Neither test reproduced the defect named in its own issue. #1838's repro is a
// `yield return` generator bound to a LOCAL (the direct-call form always
// worked); #1839's repro is a field store, with no for-in involved at all.
//
// The #1838 test below is rewritten from the issue's own repro, at a class
// element type as stdlib §4.13 requires ("the owning-enumeration interfaces
// require T to be a class type, because iteration transfers ownership of each
// element"). The #1839 test is REMOVED rather than repaired: its underlying
// defect is still live on main — the issue's own repro segfaults today
// (exit 139) — so there is no passing assertion to write. See issue #1839,
// reopened with the confirmed repro.

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
// #1838 / §29.2: a `for var x in e` loop whose source `e` is a LOCAL typed
// `IOwningEnumerable<T>` must select the vtable protocol
// (GetOwningEnumerator()/TakeCurrent()), not the array protocol.
//
// Pre-fix, `checkPureIEnumerable` matched only headName() == "IEnumerable", so
// an IOwningEnumerable-typed local fell through to isArrayBacked = true and
// bz_array_len read the vtable-backed iterator frame as a bz_array_t* — length
// 0, so the loop body never ran and the method returned 0. The direct-call form
// (`for var x in C.Gen()`) went down the vtable path and was unaffected, which
// is why the source must be bound to a LOCAL here for the test to bite.
//
// FAIL-WITHOUT: returns 0 (zero iterations). PASS-WITH: returns 30.
//
// The iteration variable is owned (iteration transfers ownership per §29.2), so
// each element is discharged with `using var y := owned x;`. `var e` draws the
// expected §28.3 auto-drop warning — a warning, not an error, and not part of
// what this test asserts.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_owning_enumerable_1838, local_owning_enumerable_source_enumerates) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  public id: int; "
        "  public constructor(x: int) id := x; end constructor; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C "
        "  public static method Gen(): IOwningEnumerable<R> "
        "    yield return new R(10); "
        "    yield return new R(20); "
        "  end Gen; "
        "end C; "
        "class M2 public static method Main(): int "
        "  var total: int := 0; "
        "  var e := C.Gen(); "
        "  for var x in e do "
        "    using var y := owned x; "
        "    total := total + y.id; "
        "  end; "
        "  return total; "
        "end Main; end M2;"), 30);
}
