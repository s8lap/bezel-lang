// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Spec §5.1 (Implicit conversions), §5.2 (Explicit conversions), §23.1
// (Auto-properties), §9.2 (Record bodies), §22.13 (Static initialization
// order), v0.240 — issue #2926.
//
// THE DEFECT. A field / auto-property INITIALIZER was never checked for
// assignability — not incompletely, ABSENT. `ExprTC`'s `FieldDecl` arm visited
// the initializer, applied §4.2's wide-literal hint and §17.7's typeless-`new`
// handling, and stopped; the LOCAL declaration arm it otherwise mirrors ends in
// exactly the `checkAssignable` this file now pins. MEASURED on 618c0c8, every
// line below COMPILED, and the first one ran and printed 44.
//
// §5.1, verbatim (v0.240): "**Constant narrowing.** An expression of integer
// type that is a **compile-time constant** ([§17.14]) implicitly converts to
// any integer type whose range contains its value: `var b: byte := 50;`,
// `Set(level: byte)` called as `Set(3)`, and enum constants of the underlying
// type ([§10.2]) are all well-typed by this rule. A constant whose value does
// not fit the target's range is a **compile error** at the conversion site —
// never a silent truncation. The rule applies to constants only; a run-time
// `int` value still requires the explicit `byte(x)` narrowing of [§5.2].
// Collection-literal element typing ([§15.4]) and default-argument typing use
// this same rule."
//
// #2903 fixed the CODEGEN half of the same gap — the store now reaches the slot
// at the SLOT's width, so case (1) truncates CONSISTENTLY at every spelling
// instead of also corrupting the neighbouring field. The missing REJECT is this
// one. The last case (`_s: string := 5`) is here because the hole admitted ANY
// type mismatch a field initializer can express, not only numeric ones — the
// check was absent, not merely incomplete.
module F2926;

class Src
  // A `static readonly` int is a RUN-TIME value, not a §17.14 compile-time
  // constant, so §5.1's constant-narrowing rule does not reach it and §5.2's
  // explicit `byte(x)` is required.
  public static readonly N: int := 200;
end Src;

// (1) A RECORD BODY field (§9.2) — the same arm, reached through a record.
record Rec
  public wl: byte := 300;
  // CHECK: :[[@LINE-1]]:{{[0-9]+}}: the initializer for field `wl` has a type that is not implicitly convertible to the target type (§5.1)
end Rec;

class Box
  // (2) #2926's own repro: an out-of-range constant that read back 44.
  public _over: byte := 300;
  // CHECK: :[[@LINE-1]]:{{[0-9]+}}: the initializer for field `_over` has a type that is not implicitly convertible to the target type (§5.1)

  // (3) A negative constant against `ulong`, whose range holds no negative.
  public _neg: ulong := -1;
  // CHECK: :[[@LINE-1]]:{{[0-9]+}}: the initializer for field `_neg` has a type that is not implicitly convertible to the target type (§5.1)

  // (4) A NON-CONSTANT `int` — §5.1: "a run-time `int` value still requires
  // the explicit `byte(x)` narrowing of [§5.2]".
  public _q: byte := Src.N;
  // CHECK: :[[@LINE-1]]:{{[0-9]+}}: the initializer for field `_q` has a type that is not implicitly convertible to the target type (§5.1)

  // (5) §5.1's two rules composed through one `?` (`S → T → T?`) — the
  // composition does not widen the RANGE, so an out-of-range constant is still
  // the compile error.
  public _bq: byte? := 300;
  // CHECK: :[[@LINE-1]]:{{[0-9]+}}: the initializer for field `_bq` has a type that is not implicitly convertible to the target type (§5.1)

  // (6) A non-numeric mismatch — the check was ABSENT, so this compiled too.
  public _s: string := 5;
  // CHECK: :[[@LINE-1]]:{{[0-9]+}}: the initializer for field `_s` has a type that is not implicitly convertible to the target type (§5.1)

  // (7) An AUTO-PROPERTY initializer (§23.1) — the compiler-generated backing
  // field is initialized at the same construction-time step (§11.1 names
  // "field and property initializers" together), so it carries the same rule.
  public property P: byte := 300;
  // CHECK: :[[@LINE-1]]:{{[0-9]+}}: the initializer for property `P` has a type that is not implicitly convertible to the target type (§5.1)

  // (8) A `static readonly` field (§22.13), whose initializer is BAKED into the
  // backing global rather than stored at construction.
  public static readonly SR: byte := 300;
  // CHECK: :[[@LINE-1]]:{{[0-9]+}}: the initializer for field `SR` has a type that is not implicitly convertible to the target type (§5.1)
end Box;

class M
  public static method Main(): int
    return 0;
  end Main;
end M;
