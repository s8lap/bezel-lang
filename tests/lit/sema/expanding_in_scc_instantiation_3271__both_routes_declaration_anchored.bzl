// fix #3271 — spec §14.6 (v0.262), *Bounded instantiation closure*. Two things
// this file pins that a gtest filter on the diagnostic TEXT cannot:
//
//   1. The generic-MEMBER route reaches real `bezelc`. §14.6's graph makes
//      "each generic type is a node, and each generic member is its own node",
//      and its consequences paragraph names the call route in those terms:
//      "a call `F<Pair<T, T>>` inside `F<T>`'s body". `Node<T>`'s field below
//      is the data route (a generic-TYPE node); `Util.Depth<T>`'s self-call is
//      the member route. Before this fix the member route compiled, linked,
//      and RAN — the monomorphizer collapsed the unbounded specialization
//      family onto three mangled keys rather than diverging, so the shape did
//      not even announce itself as a hang.
//
//   2. Both diagnostics carry a REAL location. §14.6 places the rejection "at
//      the declaration holding the edge", and the anchor used to be the
//      `Node<Pair<T, T>>` AST node — which `Parser::parseType` never stamps
//      (the #1307/#2496 degenerate-span class), so every §14.6 error reported
//      `0:0`. The `{{[1-9][0-9]*}}` in each CHECK line is the load-bearing
//      part: it fails on a line number of 0.
//
// The admitted shapes (`F<T>` at a bare parameter, `F<U, T>` permutations,
// `F<int>` at a closed type, a member self-call at the enclosing type's
// parameter) are covered by the over-rejection controls in
// tests/sema/expanding_in_scc_instantiation_3271_test.cpp — this file carries
// only the two rejections, because a lit file that must not compile cannot
// also demonstrate what does.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M;

class Pair<A, B>
  public v1: A;
  public v2: B;
  public constructor(a: A, b: B)
    this.v1 := a;
    this.v2 := b;
  end constructor;
end Pair;

class Node<T>
  public owned next: Node<Pair<T, T>>?;
end Node;

static class Util
  public static method Depth<T>(x: T, n: int): int
    using var p := new Pair<T, T>(x, x);
    return Depth<Pair<T, T>>(p, n - 1);
  end Depth;
end Util;

static class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;

// CHECK-DAG: {{[1-9][0-9]*}}:{{[0-9]+}}: the type parameter `T` of generic type `Node` is substituted with a constructed type containing a type parameter of the enclosing declaration in a recursive generic instantiation: `Node<Pair<T, T>>` (this declaration) requires `Node` instantiated at a growing argument, whose instantiation closure has no bound (§14.6, #3271); a recursive generic may not nest a type parameter inside a constructed type argument
// CHECK-DAG: {{[1-9][0-9]*}}:{{[0-9]+}}: the type parameter `T` of generic method `Depth` is substituted with a constructed type containing a type parameter of the enclosing declaration in a recursive generic instantiation: `Depth<Pair<T, T>>` (this declaration) requires `Depth` instantiated at a growing argument, whose instantiation closure has no bound (§14.6, #3271); a recursive generic may not nest a type parameter inside a constructed type argument
