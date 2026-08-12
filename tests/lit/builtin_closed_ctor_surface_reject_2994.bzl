// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Spec §17.7 "The `new` operator" (v0.241, owner ruling on #2994, PR #3043) —
// STANDARD-LIBRARY BUILTINS HAVE A CLOSED CONSTRUCTOR SURFACE, verbatim:
//
//   "For a standard-library builtin type — any type the standard-library
//    document declares — the constructor surface is exactly what that document
//    declares — `new T(...)` resolves only against a declared constructor,
//    never against a compiler-supplied fallback. A builtin declaring **no**
//    constructor — `Guid` (stdlib §2.6), `IntPtr`/`UIntPtr` (stdlib §10.0) —
//    admits no `new T(...)` at any argument list; the diagnostic names the
//    declared producers … For a builtin record whose only declared constructor
//    is its positional primary constructor (`ExceptionInfo`, §31.4;
//    `ReadOnlyStringSpan`, stdlib §3.3), the undeclared parameterless form
//    `new T()` is rejected the same way — exactly as for an identically-shaped
//    user record."
//
// MEASURED ON 309356e (pre-fix): every spelling below compiled, and reached its
// value through the #2434 DEGRADE rather than a construction — `lowerNew` has
// no arm for any of them, returns nullptr, and `lowerLocalDecl`'s fallback
// stores `getNullValue(slotTy)` behind an `internal: … (degraded to a
// zero-initialized value) — #2434` message (the field-initializer shorthand
// through #1386's `internal: lowerExpr returned null …` instead). For
// `Guid`/`IntPtr` the all-zero pattern happens to BE the documented value; for
// `ExceptionInfo` and `ReadOnlyStringSpan` — whose fields are `string` — it is
// a record of NULL strings, the #2922 class.
//
// CHECK-DAG throughout: the field-initializer diagnostic is emitted while
// walking a different declaration from the method bodies, so output order is
// not source order.
//
// The ACCEPT control is tests/lit/builtin_closed_ctor_surface_accept_2994.bzl;
// the value oracles are in
// tests/codegen/builtin_closed_ctor_surface_2994_test.cpp.
module REJ2994;

class Holder
  // The §17.7 `new()` shorthand in a FIELD initializer — the same construction
  // with the type taken from the declared target.
  private _g: Guid := new();
  // CHECK-DAG: :[[@LINE-1]]:{{[0-9]+}}: `new Guid(…)` is not admitted at any argument list{{.*}}(§17.7)
end Holder;

class REJ
  // ARM 1 — `Guid` declares no constructor, so NO argument list is admitted.
  public static method GuidNone(): int
    var g := new Guid();
    // CHECK-DAG: :[[@LINE-1]]:{{[0-9]+}}: `new Guid(…)` is not admitted at any argument list{{.*}}`Guid.Empty`, `Guid.NewGuid()`, or `default(Guid)` (§17.7)
    return 0;
  end GuidNone;

  public static method GuidOneArg(): int
    var g := new Guid(1);
    // CHECK-DAG: :[[@LINE-1]]:{{[0-9]+}}: `new Guid(…)` is not admitted at any argument list
    return 0;
  end GuidOneArg;

  public static method GuidStringArg(): int
    var g := new Guid("x");
    // CHECK-DAG: :[[@LINE-1]]:{{[0-9]+}}: `new Guid(…)` is not admitted at any argument list
    return 0;
  end GuidStringArg;

  // ARM 1 — `IntPtr` / `UIntPtr` (stdlib §10.0). The diagnostic names THEIR
  // producers, not Guid's.
  public static method IntPtrNone(): int
    var p := new IntPtr();
    // CHECK-DAG: :[[@LINE-1]]:{{[0-9]+}}: `new IntPtr(…)` is not admitted at any argument list{{.*}}`IntPtr.Zero` or `default(IntPtr)` (§17.7)
    return 0;
  end IntPtrNone;

  public static method IntPtrArg(): int
    var p := new IntPtr(0);
    // CHECK-DAG: :[[@LINE-1]]:{{[0-9]+}}: `new IntPtr(…)` is not admitted at any argument list
    return 0;
  end IntPtrArg;

  public static method UIntPtrNone(): int
    var p := new UIntPtr();
    // CHECK-DAG: :[[@LINE-1]]:{{[0-9]+}}: `new UIntPtr(…)` is not admitted at any argument list{{.*}}`UIntPtr.Zero` or `default(UIntPtr)` (§17.7)
    return 0;
  end UIntPtrNone;

  // ARM 1 — the shorthand in a typed LOCAL declaration (§17.7's other
  // shorthand position).
  public static method GuidShorthand(): int
    var g: Guid := new();
    // CHECK-DAG: :[[@LINE-1]]:{{[0-9]+}}: `new Guid(…)` is not admitted at any argument list
    return 0;
  end GuidShorthand;

  // ARM 2 — the record-shaped builtins: ONLY the undeclared parameterless form.
  public static method ExceptionInfoNone(): int
    var e := new ExceptionInfo();
    // CHECK-DAG: :[[@LINE-1]]:{{[0-9]+}}: `new ExceptionInfo()` is not a declared constructor of `ExceptionInfo`{{.*}}(§17.7)
    return 0;
  end ExceptionInfoNone;

  public static method ReadOnlyStringSpanNone(): int
    var s := new ReadOnlyStringSpan();
    // CHECK-DAG: :[[@LINE-1]]:{{[0-9]+}}: `new ReadOnlyStringSpan()` is not a declared constructor of `ReadOnlyStringSpan`{{.*}}(§17.7)
    return 0;
  end ReadOnlyStringSpanNone;

  public static method Main(): int
    return 0;
  end Main;
end REJ;
