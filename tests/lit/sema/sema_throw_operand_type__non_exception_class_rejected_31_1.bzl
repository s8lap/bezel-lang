// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// (`%not` = LLVM's exit-status inverter; the compile is expected to fail —
// see tests/lit/README.md for why every reject test wraps `bezelc` in `not`.)
//
// THE POLARITY FLIP of the permissive-rule pin this file used to carry. Until
// v0.242 it was `sema_throw_operand_type__non_exception_class_ok_31_1.bzl` and
// asserted the OPPOSITE — "a plain non-`Exception` class instance is a valid
// throw operand — the #771 ruling that throw is not restricted to
// `Exception`-derived types stands (v0.182, #1568). ACCEPTED." That ruling was
// re-opened and reversed by proposal 0080 (graduated v0.243, issue #3046), so
// the file now pins the rule that replaced it, on the same program.
//
// §31.1 (v0.243), verbatim: "`throw expr` requires `expr` to be an **owned
// instance of an `Exception`-derived class type**: the operand's static type,
// nullability not admitted, must be `Exception` or a class deriving from it
// (v0.243 — the re-ruling that discharges the owner-review flag carried here
// since v0.182 ...)". Appendix C.5 states the same invariant.
//
// `Widget` derives from nothing, so `throw new Widget()` is REJECTED, and the
// diagnostic names the remedy the v0.243 migration note states ("derive the
// class from `Exception`, one token at the declaration, or throw an
// `Exception`-derived wrapper"). The `AppError : Exception` declaration in the
// same module is the accept control's subject — see
// sema_throw_operand_type__exception_derived_class_ok_31_1.bzl.

module M; interface IShape end IShape; class Circle : IShape end Circle; class Widget end Widget; class AppError : Exception public constructor() base("e"); end constructor; end AppError; record Point(x: int); enum Color Red, Green; end Color; class C public static method Run() throw new Widget(); end Run; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the thrown value must be an exception — an instance of `Exception` or a class deriving from it
// CHECK-SAME: (§31.1)
// CHECK-SAME: `Widget` does not derive from `Exception`
// CHECK-SAME: derive it from `Exception`
