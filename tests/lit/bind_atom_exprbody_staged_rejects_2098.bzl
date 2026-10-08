// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Spec §17.15 / §28.3 K8.5 / K7.4, v0.216 (#2098 review F2): the
// EXPRESSION-BODY (`=> E`) siblings of the staged binding-atom rejections.
// An expression-bodied member runs no ownership dataflow, but codegen lowers
// the K8.5 boundary unconditionally via the shared predicate — so the
// staged bounds are enforced STRUCTURALLY there
// (checkBindAtomExprBodyStaged):
//   (1) an owned clause-initializer form outside the decidable set (a
//       Task-family mint) — previously a silent Task-frame leak;
//   (2) a conditional consume — previously a not-selected-path leak;
//   (3) a closure referencing an owned clause binding (the IIFE-consume
//       double-free shape) — conservatively rejected in `=>` position (a
//       read capture stays legal in a BLOCK body, where the precise #1958
//       dataflow scan runs).
// The legal unconditional consume (method G) compiles clean — pinned by the
// absence of a diagnostic for its line via the strict ordering below.
module M;
class Res
  public Id: int;
  public constructor(id: int) Id := id; end constructor;
  destructor Console.WriteLine("D"); end destructor;
end Res;
class W
  public static method Sink(owned r: Res): int
    var v := r.Id;
    r.Dispose();
    return v;
  end Sink;
  public static method Run(f: () -> int): int
    return f();
  end Run;
end W;
class C
  public static async method Get(): Task<int>
    return 7;
  end Get;
  public static method T(): bool
    return true;
  end T;

  // (1) Task-family mint clause in a `=>` body.
  public static method D(): int => (var t := C.Get(); 0);
  // CHECK: :[[@LINE-1]]:{{[0-9]+}}: this owned initializer form is not supported for a binding-atom clause (`t`)
  // CHECK-SAME: (§17.15 / §28.3 K8.5, v0.216)

  // (2) Conditional consume in a `=>` body.
  // §A.4 / proposal 0090 (v0.246): call-site transfer uses `move`, not `owned`.
  public static method E(): int => (var c := new Res(1); (if C.T() then W.Sink(move c) else 0));
  // CHECK: :[[@LINE-1]]:{{[0-9]+}}: binding-atom binding `c` is consumed on only some paths inside the atom
  // CHECK-SAME: (§17.15 / §28.3 K8.5, v0.216)

  // (3) A closure referencing an owned clause binding in a `=>` body (the
  // IIFE-consume shape).
  // §A.4 / proposal 0090 (v0.246): call-site transfer uses `move`, not `owned`.
  public static method F(): int => (var c := new Res(1); W.Run(lambda(): int => W.Sink(move c)));
  // CHECK: :[[@LINE-1]]:{{[0-9]+}}: a closure inside a binding atom may not reference the atom's owned binding `c` in an expression-bodied member
  // CHECK-SAME: use a block body

  // Control: the legal unconditional consume in a `=>` body compiles with no
  // diagnostic (nothing between (3)'s match and end of output names `G`).
  // §A.4 / proposal 0090 (v0.246): call-site transfer uses `move`, not `owned`.
  public static method G(): int => (var c := new Res(1); W.Sink(move c));
  // CHECK-NOT: method G
end C;
