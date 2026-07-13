// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// (`%not` = LLVM's exit-status inverter; the compile is expected to fail —
// see tests/lit/README.md for why every reject test wraps `bezelc` in `not`.)
//
// Issue #1749 / spec §12.2 (v0.184, "Hiding is not allowed"): declaring a
// member in a derived class with the same name as an inherited member
// visible at the derived class's declaration site is a compile error unless
// the declaration is an `override` of a virtual base member. This holds for
// a derived class that happens to be NESTED inside another class exactly as
// it does for a top-level derived class — §12.2 states no exception for
// nesting, and §11.6 confirms "[a] nested type is referenced from outside as
// `Outer.Nested`" with no reduced applicability of the ordinary member rules.
//
// THE BUG THIS PINS (#1749): `Resolver::checkHidingAndOverride()`
// (src/sema/TypeResolution_Checks.cpp) drove its outer walk from
// `ctx_.module()->children` only — the module's TOP-LEVEL type declarations.
// A nested class is collected as a `NestedTypeSymbol` member of its
// enclosing type (SymbolCollection.cpp), never as a `module()->children`
// entry, so the whole hiding/override check silently never ran for any
// member declared inside a nested class. `Inner` below is nested inside
// `Base` and derives from `Base`; its `public _x: int` field reuses the
// name of `Base`'s own public `_x` field with no `override` — a plain
// public-vs-public hiding violation, unrelated to the v0.184 private/nested
// visibility sub-case — and must be rejected exactly as the equivalent
// top-level shape is (see toplevel counterpart informally verified via
// bezelc during the fix; ported here as the nested repro from the issue).
module M;
class Base
  public _x: int;

  public class Inner : Base
    public _x: int;
  end Inner;
end Base;

// bezelc emits diagnostics as `file:line:col: message` with no "error:"
// severity word, so anchor on the `:line:col:` shape, the rule text, and the
// §12.2 citation (see tests/lit/ref_out_owned_local_1682.bzl for the pattern).
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member `_x` hides an inherited member of the same name
// CHECK-SAME: hiding is forbidden
// CHECK-SAME: (§12.2
