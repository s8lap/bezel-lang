// Regression tests for #8863: a function-typed local aliased into an
// `Object`-declared local (`var g: Object := f;`) leaked the closure cell
// (64 B per iteration): the bind read as an escape of `f`, while the
// `Object` slot holds no reference of its own, so nothing released the cell.
//
// Spec (v0.359):
//  - §8.1: a function type reaches `Object` via its reference-type heap
//    representation (the alias names the very cell);
//  - §28.9: runtime-managed values are released when their last owner ends.
//
// Fix: a bare `var g: Object := f;` is a forwarded bind (what `g` does decides
// what `f` may do), so `f` is released at its scope end when `g` is confined.
// A `g` that is kept (stored where it outlives `f`) keeps `f` unreleased: no
// UAF.
//
// fix-forward #8863: the forward holds only while the SOURCE keeps its value.
// A body that REASSIGNS `f` releases the old cell at the assignment, and `g`
// still names it — `var g: Object := f; f := <other lambda>;` then any use of
// `g` is a heap-use-after-free (MEASURED, `--sanitize=address`, 200
// iterations). A source the body reassigns is therefore NOT forwarded: the
// bind is an escape of `f` again (the pre-#8863 behaviour — the cell leaks,
// which is safe), the same demotion the record-holder arm (#8743) applies.
//
// per §8.1, §28.9 (v0.359)

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {
template <class R>
std::string outOf(const R& r) {
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}
}  // namespace

// The issue's repro (alias never used), 200 iterations.
BEZEL_TEST(object_alias_of_function_local_8863,
           alias_unused_released_spec_28_9) {
    BEZEL_EXPECT_EQ(outOf(runAotChecked(R"BZL(
module M;
public class Program
  public static method Main(): int
    var t := 0;
    for var i in 0 ..< 200 do
      var f := lambda() => 5;
      var g: Object := f;
      t := t + 1;
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
)BZL")), "200\n");
}

// The alias is inspected by a callee, and `f` is still callable afterwards.
BEZEL_TEST(object_alias_of_function_local_8863,
           alias_inspected_and_source_still_callable_spec_8_1) {
    BEZEL_EXPECT_EQ(outOf(runAotChecked(R"BZL(
module M;
public class Program
  public static method Take(o: Object): int
    if o = null then return 0; end;
    return 2;
  end Take;
  public static method Main(): int
    var t := 0;
    for var i in 0 ..< 200 do
      var f := lambda() => 5;
      var g: Object := f;
      t := t + Take(g) + f();
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
)BZL")), "1400\n");
}

// An `Object?`-declared alias, and a `const` alias.
BEZEL_TEST(object_alias_of_function_local_8863,
           nullable_and_const_alias_released_spec_28_9) {
    BEZEL_EXPECT_EQ(outOf(runAotChecked(R"BZL(
module M;
public class Program
  public static method Main(): int
    var t := 0;
    for var i in 0 ..< 200 do
      var f := lambda() => 5;
      var g: Object? := f;
      const h: Object := f;
      if g <> null and h <> null then t := t + f(); end;
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
)BZL")), "1000\n");
}

// The alias is stored where it outlives `f`'s scope, and the STORED closure
// is still CALLABLE afterwards: the stored value is the very cell `f` held
// (§8.1), so a fix that released the cell while the holder still names it
// shows up as a use-after-free in the call below — a null check could not see
// it. The holder's field is function-typed, so the store takes a reference of
// its own (§28.9) and the program is leak-free; without the fix the alias's
// bind reads as an ESCAPE of `f`, `f` is never released, and the row is red
// on the base for that leak.
BEZEL_TEST(object_alias_of_function_local_8863,
           stored_alias_keeps_source_no_uaf_spec_28_9) {
    BEZEL_EXPECT_EQ(outOf(runAotChecked(R"BZL(
module M;
public class Holder
  public Keep: ((int) -> int)?;
  public constructor() end constructor;
end Holder;
public class Program
  public static method Main(): int
    var h := new Holder();
    for var i in 0 ..< 50 do
      var f := lambda(x: int) => x + 6;
      var g: Object := f;
      h.Keep := g as (int) -> int;
    end for;
    var fn := h.Keep!;
    Console.WriteLine(fn!(1));
    return 0;
  end Main;
end Program;
)BZL")), "7\n");
}

// fix-forward #8863 (§8.1 / §28.9, v0.359): the alias outlives the SOURCE's
// reassignment. `f` is reassigned, so the reassignment releases the cell the
// alias still names; the alias must not be a forwarded bind there, or the
// call through it is a heap-use-after-free. The alias ESCAPES here (into an
// `Array<Object>`), so the value it keeps is released by no slot: the program
// leaks the cell (the #5866 family — a collection store keeps the pointer and
// takes no reference), which is why this row runs under the lenient oracle.
//
// CONTROL: green on the base too (the base never freed the cell — it leaked
// it), RED under the rejected commit f0b4a3a19, which forwarded the bind
// without demoting the reassigned source (heap-use-after-free). Named wrong
// fix: f0b4a3a19's `bareObjectBind` without the `assignedLocals` check.
BEZEL_TEST(object_alias_of_function_local_8863,
           reassigned_source_alias_escaping_stays_callable_spec_28_9) {
    BEZEL_EXPECT_EQ(outOf(runAotCheckedLenient(R"BZL(
module M;
public class C
  public static method Fill(arr: Array<Object>, k: int)
    var f := lambda(x: int) => x + k;
    var g: Object := f;
    f := lambda(x: int) => x - k;
    arr.Add(g);
  end Fill;
  public static method Main(): int
    var t := 0;
    for var i in 0 ..< 200 do
      var arr := new Array<Object>();
      Fill(arr, i);
      var fn := arr[0] as (int) -> int;
      t := t + fn!(1);
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
)BZL")), "20100\n");
}

// fix-forward #8863: the same shape, but the alias stays INSIDE the scope
// (only a borrow of it is taken). The confined alias must still name a live
// cell after the source's reassignment.
//
// CONTROL: green on the base too, RED under the rejected commit f0b4a3a19
// (heap-use-after-free). Named wrong fix: f0b4a3a19's `bareObjectBind`
// without the `assignedLocals` check.
BEZEL_TEST(object_alias_of_function_local_8863,
           reassigned_source_alias_confined_stays_callable_spec_28_9) {
    BEZEL_EXPECT_EQ(outOf(runAotCheckedLenient(R"BZL(
module M;
public class C
  public static method Main(): int
    var t := 0;
    for var i in 0 ..< 200 do
      var f := lambda(x: int) => x + i;
      var g: Object := f;
      f := lambda(x: int) => x - i;
      var h := g as (int) -> int;
      t := t + h!(1) + f(1);
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
)BZL")), "400\n");
}
