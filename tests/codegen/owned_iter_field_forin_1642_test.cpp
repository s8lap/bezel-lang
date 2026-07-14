// Codegen/ownership — a `for var x in <field/property>` loop whose source is an
// owned `IEnumerable<T>` / `IOwningEnumerable<T>` FIELD or PROPERTY of the
// enclosing object (issue #1642, §29.2 / §28.5, spec v0.196).
//
// Two coupled defects lived in Emitter::lowerIEnumerableBuiltinFor
// (src/codegen/LlvmBackend_Stmts.cpp):
//
//   1. PROTOCOL SELECTION. The `isArrayBacked` heuristic recognised only a
//      local/parameter identifier as vtable-backed; a field/property source —
//      whether spelled bare (`for var x in seq`, an IdentifierExpr resolving to
//      a FieldSymbol/PropertySymbol) or `this.`-qualified (`for var x in
//      this.seq`, a MemberAccessExpr) — defaulted to the raw `bz_array_t`
//      protocol. The field actually holds a vtable-backed iterator object (the
//      #1310 coercion invariant), so `bz_array_len` read the iterator frame as an
//      array → 0 iterations: a SILENT MISCOMPILE (the loop body never ran).
//
//   2. SOURCE DISPOSAL. Once (1) is corrected and the loop takes the vtable path,
//      the loop-end source disposal misclassified the field as a loop-owned
//      temporary (it recognised only local/param) and disposed it at the loop's
//      end — while the object's owned-field cascade (§28.5) ALSO disposes it at
//      the object's lifetime end: a DOUBLE-FREE / use-after-free in the class
//      DisposeCore. Per §29.2 a for-in over a NAMED owned binding (a local, an
//      `owned` parameter, or a field/property owned by the object) BORROWS the
//      source and does not assume its disposal obligation; only a freshly produced
//      temporary (a CallExpr/NewExpr result with no owning binding) is loop-owned.
//
// The fix widens both the protocol-selection and the source-disposal
// classification in lowerIEnumerableBuiltinFor to recognise a field/property
// source (bare identifier → FieldSymbol/PropertySymbol, or a member access) as a
// vtable-backed, object-owned (NOT loop-owned) source.
//
// Fails-without / passes-with: before the fix every FIELD case returns 0 (silent
// 0-element miscompile — the `== 60` assertions fail). Once the vtable path is
// taken, dropping the disposal classification reintroduces the double-free
// (heap-use-after-free in bz$Holder$DisposeCore), which runAotChecked's ASan net
// turns into r.ran==false → -999 (also failing the assertion). These tests
// therefore pin BOTH the enumeration and the exactly-once disposal.
//
// Disposal soundness: verified with runAotChecked — the generated code is
// ASan/LSan-instrumented, so a leak / double-free / use-after-free flips r.ran to
// false, which the run() wrapper maps to -999 (tests/README.md).
//
// Spec: §29.2 (Desugaring of `for` — a named/owned source is borrowed, pinned,
// and NOT disposed by the loop; only a fresh temporary gets the disposing
// `using var __source`), §28.5 (an owned field is auto-disposed by the object's
// cascade at the object's disposal). Spec revision v0.196.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// runAotChecked runs the linked executable under ASan/LSan; a leak / double-free
// / use-after-free flips r.ran to false → -999 (distinct from any legal value).
int run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// §29.2 / §28.5 (#1642): the canonical repro — a class holding an `owned
// IEnumerable<int>` field initialized from a yield-method result, iterated with a
// BARE-field for-in (`for var x in seq`). Before the fix the loop misread the
// iterator frame as a bz_array and ran zero iterations (returned 0); after the
// fix it enumerates 10+20+30 and the field is disposed exactly once at the
// holder's disposal (ASan-clean).
BEZEL_TEST(codegen_owned_iter_field_forin_1642,
           ienumerable_bare_field_forin_enumerates_and_disposes_once) {
    std::string src =
        "module M; "
        "class C public static method Gen(): IEnumerable<int> "
        "  yield return 10; yield return 20; yield return 30; "
        "end Gen; end C; "
        "class Holder "
        "  private owned seq: IEnumerable<int>; "
        "  public constructor(owned s: IEnumerable<int>) seq := s; "
        "  end constructor; "
        "  public method Sum(): int "
        "    var total: int := 0; "
        "    for var x in seq do total := total + x; end for; "
        "    return total; "
        "  end Sum; end Holder; "
        "class M2 public static method Main(): int "
        "  using var h := new Holder(C.Gen()); return h.Sum(); "
        "end Main; end M2;";
    BEZEL_EXPECT_EQ(run(src), 60);       // enumerated + disposed once (was 0)
}

// §29.2 / §28.5 (#1642): the `this.`-qualified spelling (`for var x in this.seq`,
// a MemberAccessExpr) is the same object-owned field source and must behave
// identically to the bare-field form above.
BEZEL_TEST(codegen_owned_iter_field_forin_1642,
           ienumerable_this_qualified_field_forin_enumerates_and_disposes_once) {
    std::string src =
        "module M; "
        "class C public static method Gen(): IEnumerable<int> "
        "  yield return 10; yield return 20; yield return 30; "
        "end Gen; end C; "
        "class Holder "
        "  private owned seq: IEnumerable<int>; "
        "  public constructor(owned s: IEnumerable<int>) seq := s; "
        "  end constructor; "
        "  public method Sum(): int "
        "    var total: int := 0; "
        "    for var x in this.seq do total := total + x; end for; "
        "    return total; "
        "  end Sum; end Holder; "
        "class M2 public static method Main(): int "
        "  using var h := new Holder(C.Gen()); return h.Sum(); "
        "end Main; end M2;";
    BEZEL_EXPECT_EQ(run(src), 60);
}

// §29.2 / §28.5 (#1642): the issue's named case — an `owned IOwningEnumerable<T>`
// field iterated with a bare-field for-in. Each yielded Cell is OWNED by the
// loop variable and discharged in the body; the enumerator clone is disposed at
// the loop end and the source field is disposed exactly once at the holder's
// disposal. A counting destructor (Cell.n) makes the exactly-once disposal
// observable: after the holder leaves scope, Cell.n == 0. The returned value
// (sum 60 + Cell.n 0) is 60. Before the fix: 0 (silent miscompile). Without the
// disposal-classification half of the fix: heap-use-after-free at disposal → -999.
BEZEL_TEST(codegen_owned_iter_field_forin_1642,
           iowning_enumerable_field_forin_disposes_each_exactly_once) {
    std::string src =
        "module M; "
        "class Cell public static n: int := 0; "
        "  public v: int; "
        "  public constructor(x: int) v := x; Cell.n := Cell.n + 1; "
        "  end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; end Cell; "
        "class C public static method Gen(): IOwningEnumerable<Cell> "
        "  yield return new Cell(10); yield return new Cell(20); "
        "  yield return new Cell(30); "
        "end Gen; end C; "
        "class Holder "
        "  private owned seq: IOwningEnumerable<Cell>; "
        "  public constructor(owned s: IOwningEnumerable<Cell>) seq := s; "
        "  end constructor; "
        "  public method Sum(): int "
        "    var total: int := 0; "
        "    for var x in seq do total := total + x.v; x.Dispose(); end for; "
        "    return total; "
        "  end Sum; end Holder; "
        "class M2 public static method Main(): int "
        "  var s: int := 0; "
        "  do using var h := new Holder(C.Gen()); s := h.Sum(); end do; "
        "  return s + Cell.n; "          // 60 + 0 (every Cell disposed once)
        "end Main; end M2;";
    BEZEL_EXPECT_EQ(run(src), 60);
}

// §29.2 (#1642) CONTROL: a for-in over a freshly produced OWNED TEMPORARY (a
// CallExpr result with no owning binding) must still dispose the source at the
// loop end — the widened classification must not regress the temporary path into
// a leak. Each owned Cell element is discharged in the body; the source frame is
// reclaimed at the loop end. ASan-clean, value 30 (10+20 sum + 0 residual Cells).
BEZEL_TEST(codegen_owned_iter_field_forin_1642,
           owned_temporary_source_still_disposed_once_control) {
    std::string src =
        "module M; "
        "class Cell public static n: int := 0; "
        "  public v: int; "
        "  public constructor(x: int) v := x; Cell.n := Cell.n + 1; "
        "  end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; end Cell; "
        "class C public static method Gen(): IOwningEnumerable<Cell> "
        "  yield return new Cell(10); yield return new Cell(20); "
        "end Gen; end C; "
        "class M2 public static method Main(): int "
        "  var total: int := 0; "
        "  for var x in C.Gen() do total := total + x.v; x.Dispose(); end for; "
        "  return total + Cell.n; "      // 30 + 0
        "end Main; end M2;";
    BEZEL_EXPECT_EQ(run(src), 30);
}

}  // namespace
