// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// (`%not` = LLVM's exit-status inverter; the compile is expected to fail —
// see tests/lit/README.md for why every reject test wraps `bezelc` in `not`.)
//
// §31.2.1 (v0.243, proposal 0080, #3046) — the arm-type rule, verbatim:
// "**Arm types are `Exception`-family.** Every class type named in an arm
// pattern — bare, binding, or listed in a multi-type alternative — must be
// `Exception` or a class deriving from it ... With every throwable
// `Exception`-derived ([§31.1]) and inheritance single, an arm naming any other
// class type is **provably dead** — every base of a throwable's class other
// than `Object` lies on its own chain through `Exception`, so no thrown
// instance can ever match — and is a **compile error** naming this rule".
// Appendix C.7 states the same invariant.
//
// All three named-position spellings are exercised on the SAME class so the
// check cannot be passing for one shape and silently absent for another:
//   * a BARE type arm            (`Dog =>`)
//   * a BINDING arm              (`var d: Dog =>`)
//   * a multi-type ALTERNATIVE   (`IOException | Dog =>`)

module M3046A;
import System.IO;

class Dog end Dog;

class C
  public static method Bare(): int
    try
      throw new ArgumentException("x");
    catch
      Dog => do return 1; end,
      var e: Exception => do return 2; end
    end;
    return 0;
  end Bare;

  public static method Binding(): int
    try
      throw new ArgumentException("x");
    catch
      var d: Dog => do return 1; end,
      var e: Exception => do return 2; end
    end;
    return 0;
  end Binding;

  public static method Alternative(): int
    try
      throw new ArgumentException("x");
    catch
      IOException | Dog => do return 1; end,
      var e: Exception => do return 2; end
    end;
    return 0;
  end Alternative;

  public static method Main(): int
    return 0;
  end Main;
end C;

// bezelc emits diagnostics as `file:line:col: message` with no "error:"
// severity word, so anchor on the `:line:col:` shape plus the message text.
// The line/column patterns exclude 0, so a regression to `0:0` fails the cell.

// CHECK: :{{[1-9][0-9]*}}:{{[1-9][0-9]*}}: `catch` arm type `Dog` is not an exception type
// CHECK-SAME: (§31.2.1)
// CHECK-SAME: provably dead

// CHECK: :{{[1-9][0-9]*}}:{{[1-9][0-9]*}}: `catch` arm type `Dog` is not an exception type

// CHECK: :{{[1-9][0-9]*}}:{{[1-9][0-9]*}}: `catch` arm type `Dog` is not an exception type
