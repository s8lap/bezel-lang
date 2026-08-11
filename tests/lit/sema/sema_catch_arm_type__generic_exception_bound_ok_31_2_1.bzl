// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// THE ACCEPT CONTROL for §31.2.1's generic arm-parameter rule (v0.243,
// proposal 0080, #3046). The reject half — unconstrained, value-constrained,
// `where T: class`-only, and a non-`Exception`-family class bound — is
// sema_catch_arm_type__generic_bound_required_31_2_1.bzl.
//
// §31.2.1, verbatim: "The parameter must carry an **`Exception`-derived class
// bound** (`where TEx: Exception`, or a class deriving from it ...)" and the
// bound is "decidable from the enclosing declaration's constraint set alone,
// including through `where TEx: U` chains ([§14.2])".
//
// Three admitted shapes, so the predicate is pinned as EXACTLY the clause and
// not merely "the literal token `Exception`":
//   * `where TA: Exception`                       — the built-in root itself
//   * `where TB: AppError` (AppError: Exception)  — a class deriving from it
//   * `where TC: TU`, `where TU: Exception`       — through a `where T: U` chain
//
// Clean-compile ACCEPT control: `bezelc -c` must exit zero and emit no
// diagnostics.

module M3046E;

class AppError: Exception
  public constructor() base("app"); end constructor;
end AppError;

class H
  public static method Root<TA>(): int
    where TA: Exception
    try
      throw new ArgumentException("x");
    catch
      var a: TA => do return 1; end,
      var e: Exception => do return 2; end
    end;
    return 0;
  end Root;

  public static method Derived<TB>(): int
    where TB: AppError
    try
      throw new AppError();
    catch
      var b: TB => do return 1; end,
      var e: Exception => do return 2; end
    end;
    return 0;
  end Derived;

  public static method Chained<TC, TU>(): int
    where TC: TU
    where TU: Exception
    try
      throw new ArgumentException("x");
    catch
      var c: TC => do return 1; end,
      var e: Exception => do return 2; end
    end;
    return 0;
  end Chained;
end H;

class C
  public static method Main(): int
    return 0;
  end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: must carry an `Exception`-derived class bound
// NODIAG-NOT: §
