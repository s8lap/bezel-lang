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
// A `g` that is kept (stored in a field) keeps `f` unreleased: no UAF.
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

// UAF guard: the alias is stored in a field that outlives `f`'s scope; the
// stored cell is still alive afterwards (no release of `f` while kept).
BEZEL_TEST(object_alias_of_function_local_8863,
           stored_alias_keeps_source_no_uaf_spec_28_9) {
    BEZEL_EXPECT_EQ(outOf(runAotCheckedLenient(R"BZL(
module M;
public class Holder
  public Keep: Object?;
  public constructor() end constructor;
  public method Store(o: Object) this.Keep := o; end Store;
end Holder;
public class Program
  public static method Main(): int
    var h := new Holder();
    for var i in 0 ..< 50 do
      var f := lambda() => 6;
      var g: Object := f;
      h.Store(g);
    end for;
    Console.WriteLine(h.Keep <> null);
    return 0;
  end Main;
end Program;
)BZL")), "True\n");
}
