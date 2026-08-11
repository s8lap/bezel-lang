// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// (`%not` = LLVM's exit-status inverter; the compile is expected to fail —
// see tests/lit/README.md for why every reject test wraps `bezelc` in `not`.)
//
// §31.2.1 (v0.243, proposal 0080, #3046) — the generic arm-parameter rule,
// verbatim: "The parameter must carry an **`Exception`-derived class bound**
// (`where TEx: Exception`, or a class deriving from it — the formerly idiomatic
// form is, since v0.243, the required one) so that every substitution is a
// throwable class ([§31.1]); a parameter that is unconstrained,
// value-constrained, bounded only by `where TEx: class` (which admits `string`
// and function types), or bounded by a class outside the `Exception` family is
// a compile error at the arm, **checked at the generic declaration** — whether
// the parameter carries such a bound is decidable from the enclosing
// declaration's constraint set alone, including through `where TEx: U` chains
// ([§14.2]), so the arm's well-formedness never depends on an instantiation".
// Appendix C.7 states the same invariant.
//
// The four rejected bound shapes the clause enumerates, one method each. The
// ACCEPT half — `where TEx: Exception`, a derived class bound, and a
// `where TEx: U` chain that reaches one — is
// sema_catch_arm_type__generic_exception_bound_ok_31_2_1.bzl.
//
// NOTE the "checked at the generic declaration" half: none of these methods is
// ever CALLED, so a check that only ran per instantiation would report nothing
// at all here.

module M3046D;

class Dog end Dog;

class H
  // (1) unconstrained
  public static method Unbounded<TA>(): int
    try
      throw new ArgumentException("x");
    catch
      var a: TA => do return 1; end,
      var e: Exception => do return 2; end
    end;
    return 0;
  end Unbounded;

  // (2) value-constrained
  public static method ValueBound<TB>(): int
    where TB: value
    try
      throw new ArgumentException("x");
    catch
      var b: TB => do return 1; end,
      var e: Exception => do return 2; end
    end;
    return 0;
  end ValueBound;

  // (3) `where TC: class` only — §14.2 admits `string` and function types under
  // it, so it cannot guarantee a throwable class.
  public static method ClassOnly<TC>(): int
    where TC: class
    try
      throw new ArgumentException("x");
    catch
      var c: TC => do return 1; end,
      var e: Exception => do return 2; end
    end;
    return 0;
  end ClassOnly;

  // (4) bounded by a class OUTSIDE the `Exception` family.
  public static method NonExceptionBound<TD>(): int
    where TD: Dog
    try
      throw new ArgumentException("x");
    catch
      var d: TD => do return 1; end,
      var e: Exception => do return 2; end
    end;
    return 0;
  end NonExceptionBound;
end H;

class C
  public static method Main(): int
    return 0;
  end Main;
end C;

// CHECK: :{{[1-9][0-9]*}}:{{[1-9][0-9]*}}: the `catch` arm's type parameter `TA` must carry an `Exception`-derived class bound
// CHECK-SAME: (§31.2.1)

// CHECK: :{{[1-9][0-9]*}}:{{[1-9][0-9]*}}: the `catch` arm's type parameter `TB` must carry an `Exception`-derived class bound

// CHECK: :{{[1-9][0-9]*}}:{{[1-9][0-9]*}}: the `catch` arm's type parameter `TC` must carry an `Exception`-derived class bound

// CHECK: :{{[1-9][0-9]*}}:{{[1-9][0-9]*}}: the `catch` arm's type parameter `TD` must carry an `Exception`-derived class bound
