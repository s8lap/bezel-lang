// fix #3719 (§17.11 / §3.6 rule 2 / §14.2 / §26.4, v0.294): the EXTENSION-level
// half of the class-level fix beside this file
// (default_enum_zero_3719__class_level_generic_value_instantiation_rejected.bzl).
// `extension Box<T> where T: value` introduces its OWN block-level type
// parameter `T` (§26.4) — a different AST identity from `Box<K>`'s own class-
// level `K` — and its `default(T)` obligation is owned by the extension's own
// `ast::ExtensionDecl*`, not by `Box`'s `TypeDecl*`.
//
// Getting this diagnostic to fire needed TWO fixes, not one:
//
//  1. (Shared with the class-level half.) `recordGenericInstantiationSite`
//     was only ever called with a called METHOD's own owner; construction
//     (`new Box<Level>()`) now also records one instantiation site per LOCAL
//     extension targeting `Box` at matching arity, keyed by that extension's
//     own `ExtensionDecl*` — see recordDefaultObligationsForConstruction.
//
//  2. (Extension-only, and NOT part of #3719's original filed scope — a
//     second, orthogonal defect this fix also closes because otherwise fix
//     1 alone is inert here.) The `default(T)`-under-`where T: value`
//     RECORDING code itself (ExprTypecheck_Exprs.cpp) used to be an
//     `else if` chain: `if (isMethodParam) {...} else if (currentType_ &&
//     currentType_->decl) {...} else if (currentExtension_ && ...) {...}`.
//     Inside an extension body over a LOCAL target type, `currentType_` is
//     ALWAYS non-null (`ExprTC::visitTopDecl`'s ExtensionDecl arm sets both
//     `currentType_ = targetSym` and `currentExtension_ = extSym`), so the
//     `currentType_` branch unconditionally consumed the chain — and since
//     `Box`'s own (empty) `genericConstraints` never mentions the
//     extension's own parameter name (`T`), that branch's inner check
//     silently failed WITHOUT falling through to try the `currentExtension_`
//     arm #3644 added. The extension's own obligation was therefore never
//     even RECORDED for any extension over a local target whose own
//     parameter name did not name-coincide with the class's — the common
//     case. Fixed by trying both scopes independently (mirroring the
//     already-correct sibling `checkConstraintsFor`, a few lines up in the
//     same function, which #3644 already got right for the non-null-
//     constraint diagnostic).
//
// `Level` declares 1, 2, 3 (no zero-valued constant); `Box` itself declares
// NO constraint on `K` at all (so a name-coincidence with the extension's own
// `T` cannot be the reason this fires).
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M;

enum Level Low := 1, Mid := 2, High := 3; end Level;

class Box<K>
  public V: K?;
end Box;

extension Box<T> where T: value
  public method IsZero(): bool
    var t := default(T);
    return t = t;
  end IsZero;
end;

class C
  public static method Main(): int
    var b := new Box<Level>();
    return 0;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `Level` does not satisfy the constraint `where T: value`
// CHECK-SAME: takes `default(T)`
