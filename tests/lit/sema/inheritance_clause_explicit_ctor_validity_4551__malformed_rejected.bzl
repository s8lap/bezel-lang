// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// (`%not` = LLVM's exit-status inverter: the compile is EXPECTED to fail, and
// wrapping it in `not` lets its diagnostics flow through lit's pipe to
// FileCheck — see tests/lit/lit.cfg.py and README.md.)
//
// Issue #4551 / spec §22.5 "Optional parameters and named arguments" and
// §22.10 "Primary constructors" (v0.306): §22.5 argument-binding VALIDITY was
// unchecked for a §22.10 inheritance clause binding an EXPLICIT base
// constructor — the third site's twin of #4496 (the `base(...)` chain) and
// #4518 (the clause bound to the base's PRIMARY parameter list).
//
// This is the DIAGNOSTIC track. The four messages are not new text — they are
// the strings `bindArgsToParams` already emits at every other construction
// site — so what this file adds over the behaviour track is the LOCATION each
// rejection reports at, pinned to the exact column (a `{{[0-9]+}}` wildcard
// would match a diagnostic elsewhere on the same line and pin nothing).
//
// Every clause below reports at its first argument's VALUE expression — the span
// sema hands the binder for the whole clause (so a NAMED argument reports at
// the value, not at the name). That is the same convention the `base(...)`
// chain arm uses for its own call.
//
// The ACCEPT controls — a well-formed named clause, an all-positional one, a
// clause omitting a §22.5-defaulted parameter, a clause binding the base's
// primary list, one binding a convenience constructor, and an overload set —
// are in tests/sema/inheritance_clause_explicit_ctor_validity_4551_test.cpp,
// where the printed VALUES can be asserted as well as acceptance.

module M;

class B
  public n: int := 0;
  public s: string := "";
  public constructor(a: int, b: string)
    n := a;
    s := b;
  end constructor;
end B;

// §22.5: a named argument names an existing parameter. Pre-fix this one was
// not merely undiagnosed — the argument supplied `a` BY POSITION.
class DNoSuch(v: int) : B(zz := v, b := "e")
// CHECK: :[[@LINE-1]]:33: inheritance-clause base call to a constructor of `B` has no parameter named `zz` (§22.5)
end DNoSuch;

// §22.5: "naming the same parameter twice, is a compile error at the call
// naming that parameter".
class DTwice(v: int) : B(a := v, a := 2)
// CHECK: :[[@LINE-1]]:31: inheritance-clause base call to a constructor of `B`: parameter `a` is bound more than once (§22.5)
end DTwice;

// §22.5: "naming a parameter already bound positionally" — the other half of
// the same sentence.
class DPosThenName(v: int) : B(v, a := 2, b := "e")
// CHECK: :[[@LINE-1]]:32: inheritance-clause base call to a constructor of `B`: parameter `a` is bound more than once (§22.5)
end DPosThenName;

// Over-supplied: three arguments against a two-formal constructor.
class DOver(v: int) : B(v, "e", 3)
// CHECK: :[[@LINE-1]]:25: inheritance-clause base call to a constructor of `B` supplies 3 argument(s) but the parameter list has 2 (§22.5)
end DOver;

// §22.5: "Every parameter that declares no default must be bound,
// positionally or by name."
class DUnder(v: int) : B(v)
// CHECK: :[[@LINE-1]]:26: inheritance-clause base call to a constructor of `B`: required parameter `b` is not supplied (§22.5)
end DUnder;

class C
  public static method Main(): int
    return 0;
  end Main;
end C;
