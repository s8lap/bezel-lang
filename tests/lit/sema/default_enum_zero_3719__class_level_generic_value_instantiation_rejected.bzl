// fix #3719 (§17.11 / §3.6 rule 2 / §14.2, v0.294): §17.11's closing rule — "A
// substitution whose `default` is not well-defined ... is rejected at the
// instantiation site (the §3.6 rule 2 pattern)" — applies to a CLASS-level
// `where K: value` type parameter, not just a method-level one.
//
// Before this fix, `recordGenericInstantiationSite` (ExprTypecheck.cpp) was
// called ONLY from a generic METHOD's own call site
// (ExprTypecheck_Calls.cpp's checkMethodGenericConstraints), always with
// `owner = md` (the called method's own MethodDecl*). A `default(K)`
// obligation recorded here with `owner = currentType_->decl` (Box's
// TypeDecl*, since `K` is `Box<K>`'s own class-level parameter, not a method
// parameter) could therefore never match any recorded instantiation site —
// dead code since #3044 introduced the class-level recording arm. This fix
// teaches the object-construction path (`new Box<Level>()`) to also record
// an instantiation site under `Box`'s own TypeDecl*, so the join fires here
// exactly as it already did for the method-level shape
// (default_enum_zero_3044__generic_value_instantiation_rejected.bzl).
//
// `Level` declares 1, 2, 3 (no zero-valued constant), so `default(Level)` is
// a compile error (§17.11) and §17.11's "under `where T: value`, `default(T)`
// is always non-null" guarantee cannot hold for `Box<Level>` — so the
// INSTANTIATION `new Box<Level>()` is rejected, at the instantiation site,
// even though `Box<K>`'s own body type-checks "under the weakest assumption
// its constraints establish" and `Get()` itself is never even called here.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M;

enum Level Low := 1, Mid := 2, High := 3; end Level;

class Box<K> where K: value
  public method Get(): K
    var t := default(K);
    return t;
  end Get;
end Box;

class C
  public static method Main(): int
    var b := new Box<Level>();
    return 0;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `Level` does not satisfy the constraint `where K: value`
// CHECK-SAME: takes `default(K)`
