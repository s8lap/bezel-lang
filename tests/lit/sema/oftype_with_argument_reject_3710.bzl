// fix #3710 (stdlib §5 `System.Collections.Linq`, v0.286) — a WITHDRAWN §5
// member given ANY argument used to escape both the "no member" reject and
// the removal diagnostic.
//
// `OfType<U>()` was REMOVED from the documents at v0.284 (fix #3559 taught
// the compiler this), but both sema arms that raise the removal diagnostic
// gated on `ce.args.empty()` — while the §5 name exemption in
// `kBuiltinCollectionLinqOperatorNames` (BuiltinRegistry.cpp) that lets a
// bare `OfType` reference reach either arm at all is name-only, not
// arity-aware. So `OfType<U>(x)` was waved past the "no member" reject AND
// skipped by the removal arm: nothing else typed the call, and codegen's
// #2434/#2491 zero-init degradation notes let the program compile rc=0,
// link, and run silently — the exact loud-to-quiet shape #2935 / #2976 /
// #3155 / #3274 fixed for this family's other members.
//
// BOTH ARMS, WITH AN ARGUMENT. The collection-receiver arm (Array) and the
// lazy-receiver arm (a `Where(...)` chain) each reach `OfType` through a
// separate code path in ExprTypecheck_Calls.cpp; a fix wired into only one
// would leave the other's `(x)` cell compiling silently. This file is the
// with-argument sibling of `tests/lit/oftype_removed_v0284_3559.bzl`, which
// pins the same two arms at zero arguments.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M3710OfTypeWithArgument;

class C
  public static method Main(): int
    using var a := new Array<int>();
    a.Add(1);
    using var b := a.OfType<int>(0).ToArray();
    using var c := a.Where(lambda(x: int) => x > 0).OfType<int>(0).ToArray();
    return b.Count + c.Count;
  end Main;
end C;

// The collection-receiver arm, given an argument. Located at the call, never
// 0:0 — same wording the zero-arg cell gets, because arity is not the
// question the diagnostic is answering.
// CHECK: :{{[1-9][0-9]*}}:{{[1-9][0-9]*}}: `OfType<int>()` on a sequence of `int`
// CHECK-SAME: was REMOVED from the standard library at v0.284 (stdlib §5)
// CHECK-SAME: the operator is gone, not misspelled (issue 3559)
// CHECK-SAME: Filter explicitly with `Where` and an `is` test (spec §12.7)

// The lazy-receiver arm, given an argument — reaches the SAME diagnostic, so
// the two arms cannot drift into two spellings of one rule at nonzero arity
// either.
// CHECK: :{{[1-9][0-9]*}}:{{[1-9][0-9]*}}: `OfType<int>()` on a sequence of `int`
// CHECK-SAME: was REMOVED from the standard library at v0.284 (stdlib §5)
