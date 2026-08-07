// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Spec §5.1 (Implicit conversions — "Constant narrowing"), §17.14 (Constant
// expressions), §5.2 (Explicit conversions), §22.5 (Default values), v0.240 —
// the REJECT half of #2925.
//
// §5.1, verbatim: "A constant whose value does not fit the target's range is a
// **compile error** at the conversion site — never a silent truncation. The
// rule applies to constants only; a run-time `int` value still requires the
// explicit `byte(x)` narrowing of [§5.2]."
//
// #2925 widened the POPULATION the constant-narrowing rule is applied to, from
// three hand-written statement kinds to every §5.1 conversion site. This file
// pins the two shapes the widened rule must still REJECT at each of those newly
// reached sites — an out-of-range constant, and a non-constant `int` — so the
// fix cannot be mistaken for "narrow whatever reaches a narrower slot".
module REJ2925;

class M
  static method Take(b: byte): int
    return int(b);
  end Take;

  // (1) `return` — an out-of-range CONSTANT. §5.1: "a compile error at the
  // conversion site — never a silent truncation".
  static method RetOver(): byte
    return 300;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: the returned value has a type that is not implicitly convertible to the target type (§5.1)
  end RetOver;

  // (2) `return` — a NON-CONSTANT `int`. §5.1: "The rule applies to constants
  // only; a run-time `int` value still requires the explicit `byte(x)`".
  static method RetVar(): byte
    var n := 200;
    return n;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: the returned value has a type that is not implicitly convertible to the target type (§5.1)
  end RetVar;

  // (3) A DEFAULT ARGUMENT — an out-of-range constant. §5.1 names default
  // arguments as a site of this same rule, and #2925 gave this site its first
  // assignability check of any kind.
  static method DefOver(level: byte := 300): int
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: the default value of parameter `level` has a type that is not implicitly convertible to the target type (§5.1)
    return int(level);
  end DefOver;

  // (4) A DEFAULT ARGUMENT — a negative constant against `ulong`, whose range
  // contains no negative value.
  static method DefNeg(u: ulong := -1): int
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: the default value of parameter `u` has a type that is not implicitly convertible to the target type (§5.1)
    return int(u);
  end DefNeg;

  static method Main(): int
    var n := 200;
    // (5) A CALL ARGUMENT — an out-of-range constant.
    var a := M.Take(300);
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: the argument `b` to call to `Take` has a type that is not implicitly convertible to the target type (§5.1)
    // (6) A CALL ARGUMENT — a non-constant `int`.
    var b := M.Take(n);
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: the argument `b` to call to `Take` has a type that is not implicitly convertible to the target type (§5.1)
    return a + b;
  end Main;
end M;
