// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// (`%not` = LLVM's exit-status inverter: the compile is EXPECTED to fail, and
// wrapping it in `not` lets its diagnostics flow through lit's pipe to
// FileCheck — see tests/lit/lit.cfg.py and README.md.)
//
// Issue #4824 / spec §22.5 "Optional parameters and named arguments", §22.6,
// §12.8 "Constructor chaining" (v0.307): a `base(...)` / `this(...)` chain
// call that no candidate constructor of the target type binds is a compile
// error ("Every parameter that declares no default must be bound … the error
// is reported only where no candidate survives"). Before the fix each chain
// below compiled clean and the object stayed calloc-zero.
//
// This is the DIAGNOSTIC track: it pins the text and the location (the chain
// call's first token). The behaviour track, with accept controls asserting
// printed values, is tests/sema/ctor_chain_unbound_required_param_4824_test.cpp.

module M;

class B
  public readonly property Tag: string;
  public constructor(x: int, y: int := 0) Tag := $"B{x},{y}"; end constructor;
end B;

// The issue's repro 1: the base's required `x` is left unbound.
class K : B
  public constructor(n: int) base(y := n); end constructor;
  // CHECK: :[[@LINE-1]]:30: `base(...)` constructor chain call in `K`: required parameter `x` is not supplied (§22.5)
end K;

// The issue's repro 2: the same shape as a `this(...)` chain.
class T
  public readonly property Tag: string;
  public constructor(x: int, y: int := 0) Tag := $"T{x},{y}"; end constructor;
  public constructor(s: string, t: string) this(y := 6); end constructor;
  // CHECK: :[[@LINE-1]]:44: `this(...)` constructor chain call in `T`: required parameter `x` is not supplied (§22.5)
end T;

// #4824's comment: a name no target constructor declares.
class U
  public readonly property Tag: string;
  public constructor(x: int := 1) Tag := $"U{x}"; end constructor;
  public constructor(s: string) this(zzz := 99); end constructor;
  // CHECK: :[[@LINE-1]]:33: `this(...)` constructor chain call in `U` has no parameter named `zzz` (§22.5)
end U;

// Overloaded base, no candidate binds.
class Two
  public constructor(a: int, b: int) end constructor;
  public constructor(s: string, t: string, u: string) end constructor;
end Two;

class V : Two
  public constructor(n: int) base(n); end constructor;
  // CHECK: :[[@LINE-1]]:30: `base(...)` constructor chain call in `V`: no constructor of `Two` can bind these arguments — each candidate leaves a required parameter unbound, lacks a parameter the call names, or takes fewer arguments (§12.8, §22.5, §22.6)
end V;

class C
  public static method Main(): int
    return 0;
  end Main;
end C;
