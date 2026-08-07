// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// (`%not` = LLVM's exit-status inverter; the compile is expected to fail —
// see tests/lit/README.md for why every reject test wraps `bezelc` in `not`.)
//
// Issue #2983 / spec §12.3 (`base.Member`) / §31.4 (Standard exception types) /
// §12.1, v0.240 — the DIAGNOSTIC half. The acceptance half (`base.Message` &
// co. on a direct built-in exception subclass) is pinned with its VALUES in
// tests/codegen/base_builtin_exception_surface_2983_test.cpp.
//
// THE DEFECT. `ExprTC::resolveMemberAccess`'s `base` arm resolved `base` by
// scanning `TypeSymbol::baseTypes` for a class entry. A BUILT-IN exception has
// no `TypeSymbol` at all, so it is never in that collection — a correct
// predicate fed by a source collection that never held the population. The
// `!targetType` fallback then concluded "no class base", which is only true
// when the class really has none, and said so:
//
//     bm.bzl:5:12: `base.Message` does not name a base-class member: `ExB` has
//     no class base (its base is `Object`), and `Object` has no `Message` — ...
//
// That is a statement about the user's own declaration, and §31.4 declares
// `public class Exception`, so it is FALSE — `class ExB: Exception` has a class
// base and that base declares `Message`. This file pins that a `base.X` reject
// that is still CORRECT TO FIRE no longer says something untrue while firing.
//
// Three rejects, one accept-control-by-absence:
//   * `base.Nope` — not on any surface. Rejected, naming the REAL base.
//   * `base.ParamName` — §31.4 declares it on `ArgumentException`, NOT on
//     `Exception`, so it is correctly absent here; the message must not offer
//     it either. The surface list is built by ASKING the resolver member by
//     member, so a message can never name something the resolver would reject.
//   * `base[0]` — §31.4 declares no indexer on any built-in exception. Same
//     falsehood in the sibling `base[...]` arm (ExprTypecheck_Exprs.cpp),
//     corrected the same way.
//   * `Plain`, whose class base is genuinely absent, keeps the ORIGINAL
//     `Object` wording verbatim — the fix must narrow, not replace.
//
// Every location is a real `line:col`. The `base.NopeChain.Count` shape below
// is there because that exact spelling reported `0:0` before the fix (a
// degenerate MemberAccessExpr span, #1307/#2496).
module M2983;

class ExB: Exception
  public constructor(m: string) base(m); end constructor;
  public method Bad(): string   return base.Nope;             end Bad;
  public method Bad2(): string  return base.ParamName ?? "x"; end Bad2;
  public method Bad3(): int     return base[0];               end Bad3;
  public method Bad4(): int     return base.NopeChain.Count;  end Bad4;
end ExB;

class Plain
  public method Bad(): string   return base.Nope;             end Bad;
end Plain;

class C
  public static method Main(): int
    return 0;
  end Main;
end C;

// bezelc emits diagnostics as `file:line:col: message` with no "error:"
// severity word, so anchor on the `:line:col:` shape plus the message text.

// The reject names the ACTUAL base, and the surface it ACTUALLY has —
// Message/StackTrace/CauseChain, with ParamName correctly absent because §31.4
// declares that one on ArgumentException.
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `base.Nope` does not name a base-class member
// CHECK-SAME: `ExB`'s class base is the built-in `Exception` (§31.4)
// CHECK-SAME: whose surface is Message, StackTrace, CauseChain and the universal
// CHECK-SAME: (§12.3)

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `base.ParamName` does not name a base-class member
// CHECK-SAME: `ExB`'s class base is the built-in `Exception` (§31.4)

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `base[...]` does not name a base-class indexer
// CHECK-SAME: `ExB`'s class base is the built-in `Exception` (§31.4), which declares no indexer
// CHECK-SAME: (§12.3/§23.4)

// The degenerate-span shape: a real location, never `0:0`.
// CHECK: :{{[1-9][0-9]*}}:{{[1-9][0-9]*}}: `base.NopeChain` does not name a base-class member

// A class with a genuinely absent class base keeps the original wording.
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `base.Nope` does not name a base-class member
// CHECK-SAME: `Plain` has no class base (its base is `Object`)
// CHECK-SAME: use `this.Nope` or just `Nope`
