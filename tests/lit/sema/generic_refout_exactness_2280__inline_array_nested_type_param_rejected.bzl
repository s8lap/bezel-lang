// §14.5 / §14.2 (spec v0.236) / #2280: a `ref` parameter position unifies
// exactly even when its method type parameter is nested in an inline-array
// element type. With explicit T=int, `[4]long` cannot alias a `[4]int` location.
// Before the fix, the §14.5 domain predicate inspected generic arguments but
// not `arrayElement`, so this mismatched-width call compiled cleanly.
//
// RUN: not %bezelc -c -o %t.o %s 2>&1 | FileCheck %s
// CHECK: the `ref` argument for parameter `r` does not unify exactly
// CHECK-SAME: §14.5

module M;
static class U
  public static method Fill<T>(ref r: [4]T)
    r[0] := default(T);
  end Fill;
end U;
class C
  public static method Main(): int
    var arr: [4]long;
    U.Fill<int>(ref arr);
    return 0;
  end Main;
end C;
