// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// (`%not` = LLVM's exit-status inverter: the compile is EXPECTED to fail, and
// wrapping it in `not` lets its diagnostics flow through lit's pipe to
// FileCheck — see tests/lit/lit.cfg.py and README.md for the full rationale.)
//
// Spec §8.1 (the universal Object surface a primitive exposes) / §26.5 (member
// resolution: no instance member, no extension → compile error) / stdlib §9
// (the documented primitive instance members), v0.220, issue #2126.
//
// A member access on a PRIMITIVE-typed VALUE naming a member that is not a
// documented primitive member (stdlib §9), not a universal Object member
// (§8.1), and not provided by any §26.5 extension has no member to resolve. The
// receiver being a primitive keeps sema's `targetType` null, so the
// known-complete-user-type "no member" diagnostic never fired; the access was
// left untyped and the enclosing call fell through to codegen's unresolved-
// member default — a silent `0` result instead of an error (the exact shape
// found while fixing #2097: a dotted head like `Data.M()` resolving `Data` to
// an `int` field made `.M()` a member access on `int` that miscompiled to 0).
// Each arm below pins one axis: primitive KIND (int/char/bool/double/decimal) ×
// HEAD FORM (local / parameter / field / literal) × MEMBER KIND (method-call /
// property read).
//
// Accept controls — that real primitive members (ToString, CompareTo, Equals,
// int.Parse, char comparisons) still compile and run — live in the behavior
// track: tests/codegen/primitive_member_resolution_2126_test.cpp.
module M;
class C
  private _data: int := 0;

  // int LOCAL head, method-call member — the exact #2126 repro.
  public static method LocalHead(): int
    var x := 5;
    return x.NoSuchMethod();
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: no member `NoSuchMethod` on primitive type `int`
  end LocalHead;

  // int FIELD head, method-call member — the #2097 report's exact shape
  // (`Data.M()` where `Data` resolves to an `int` field → silent 0).
  public method FieldHead(): int
    return _data.M();
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: no member `M` on primitive type `int`
  end FieldHead;

  // int PARAMETER head, method-call member.
  public static method ParamHead(p: int): int
    return p.DoesNotExist();
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: no member `DoesNotExist` on primitive type `int`
  end ParamHead;

  // char LOCAL head, method-call member.
  public static method CharHead(): int
    var c := 'a';
    var r := c.Bogus();
    return 0;
    // CHECK: :[[@LINE-2]]:{{[0-9]+}}: no member `Bogus` on primitive type `char`
  end CharHead;

  // bool LOCAL head, method-call member.
  public static method BoolHead(): int
    var b := true;
    var r := b.Nope();
    return 0;
    // CHECK: :[[@LINE-2]]:{{[0-9]+}}: no member `Nope` on primitive type `bool`
  end BoolHead;

  // double LOCAL head, PROPERTY read (no parens) member.
  public static method PropForm(): int
    var d := 3.5;
    var q := d.NotAThing;
    return 0;
    // CHECK: :[[@LINE-2]]:{{[0-9]+}}: no member `NotAThing` on primitive type `double`
  end PropForm;

  // decimal LOCAL head, method-call member.
  public static method DecHead(): int
    var m := 1.5m;
    var r := m.Wat();
    return 0;
    // CHECK: :[[@LINE-2]]:{{[0-9]+}}: no member `Wat` on primitive type `decimal`
  end DecHead;

  // bool `CompareTo` — bool does NOT implement IComparable (§9.4), so even a
  // real-looking universal-ish comparison member has no member to resolve.
  public static method BoolCompareTo(): int
    var b := false;
    var r := b.CompareTo(true);
    return 0;
    // CHECK: :[[@LINE-2]]:{{[0-9]+}}: no member `CompareTo` on primitive type `bool`
  end BoolCompareTo;
end C;
