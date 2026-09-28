// fix #5117 (§14.2, v0.307): unannotated lambda param whose formal type is
// `Array<U>` (the method's own generic U nested inside a Named type's
// generic-arg position) silently returns wrong value.
//
// The lambda body mistypes the parameter as `Array<unresolved>`, so `y.Count`
// returns 0 instead of the actual count. #5066's `mentionsOwnGeneric` walk
// recurses into a Named's `genericArgs` and finds the method's own `U`, causing
// `formalFnParamType2` to bail — but the class-level substitution map already
// resolved `U` from the call's bound type argument (e.g., `Box<string>.Apply<int>`
// binds `U=int`). The fix applies the class-level substitution to each formal
// parameter type before checking `mentionsOwnGeneric`.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The repro from the issue: Box<string>.Apply<int> binds U=int, arr: Array<int>
// should be inferred for lambda param `y`. Without the fix, `y.Count` returns 0
// (no elements known) instead of 3.
const char kRepro[] = R"(
module Q;
public class Box<T>
  public method Apply<U>(v: T, arr: Array<U>, f: (Array<U>) -> int): int
    return f(arr);
  end Apply;
end Box;
public class Program
  public static method Main(): int
    var b := new Box<string>();
    var a := new Array<int>();
    a.Add(1); a.Add(2); a.Add(3);
    var t: int := b.Apply<int>("abc", a, lambda(y) => y.Count);
    Console.WriteLine($"<{t}>");
    return 0;
  end Main;
end Program;
)";

// Control: explicit type annotation should already work and print <3>.
const char kExplicitType[] = R"(
module Q;
public class Box<T>
  public method Apply<U>(v: T, arr: Array<U>, f: (Array<U>) -> int): int
    return f(arr);
  end Apply;
end Box;
public class Program
  public static method Main(): int
    var b := new Box<string>();
    var a := new Array<int>();
    a.Add(1); a.Add(2); a.Add(3);
    var t: int := b.Apply<int>("abc", a, lambda(y: Array<int>) => y.Count);
    Console.WriteLine($"<{t}>");
    return 0;
  end Main;
end Program;
)";

// Dictionary<string, U> shape — U in value position.
const char kDictionary[] = R"(
module Q;
public class Box<T>
  public method WithDict<U>(d: Dictionary<string, U>, f: (Dictionary<string, U>) -> int): int
    return f(d);
  end WithDict;
end Box;
public class Program
  public static method Main(): int
    var b := new Box<string>();
    var d := new Dictionary<string, int>();
    d["a"] := 1; d["b"] := 2; d["c"] := 3;
    var t: int := b.WithDict<int>(d, lambda(y) => y.Count);
    Console.WriteLine($"<{t}>");
    return 0;
  end Main;
end Program;
)";

} // namespace

// The actual defect: with the unsubstituted `Array<U>` reaching the lambda,
// `y.Count` is 0 (no elements known) instead of 3.
BEZEL_TEST(sema_5117, lambda_nested_array_own_generic_repro) {
    auto r = runAot(kRepro);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "<3>\n");
}

// Control: explicit annotation always worked.
BEZEL_TEST(sema_5117, lambda_nested_array_own_generic_explicit_type) {
    auto r = runAot(kExplicitType);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "<3>\n");
}

// Dictionary<string, U> — U in value position.
BEZEL_TEST(sema_5117, lambda_nested_dict_own_generic) {
    auto r = runAot(kDictionary);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "<3>\n");
}
