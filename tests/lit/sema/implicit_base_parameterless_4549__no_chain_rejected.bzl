// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// (`%not` = LLVM's exit-status inverter: the compile is EXPECTED to fail, and
// wrapping it in `not` lets its diagnostics flow through lit's pipe to
// FileCheck — see tests/lit/lit.cfg.py and README.md.)
//
// Issue #4549 / spec §12.8 "Constructor chaining" (v0.306): the IMPLICIT
// `base()` call the compiler inserts when a class writes no chain was never
// checked against §12.8's own requirement on it —
//
//   "If neither `base(...)` nor `this(...)` is written, the compiler inserts
//    an implicit `base()` call (which requires the base class to have a
//    parameterless constructor; otherwise the chain must be explicit)."
//
// This is the DIAGNOSTIC track: it pins the message text and, exactly, the
// LOCATION each of the three spellings reports at, which a gtest oracle
// asserting `diagnosticMessageContains` cannot. The columns are pinned
// literally (not `{{[0-9]+}}`) because a wildcard column would match a
// diagnostic on the same line at a different place and pin nothing.
//
// The ACCEPT controls — an all-defaulted primary list, an all-defaulted
// explicit constructor, an explicit parameterless constructor, a private one,
// an `abstract` base, an inheritance clause carrying arguments, a written
// `base(...)` chain and a built-in exception base — are in the behaviour
// track, tests/sema/implicit_base_parameterless_4549_test.cpp, where the
// printed VALUES can be asserted as well as acceptance.

module M;

class B(n: int, s: string)
  public method Show()
    Console.WriteLine(n.ToString());
    Console.WriteLine(s);
  end Show;
end B;

class Bx
  public n: int := 0;
  public constructor(a: int)
    n := a;
  end constructor;
end Bx;

// Spelling 1 — no constructor at all. §22.7's implicit parameterless
// constructor for `NoCtor` chains to `base()`, and the `: B` clause is where a
// reader supplies the missing arguments, so that is where the diagnostic
// lands: on the base TYPE NAME, column 16.
class NoCtor : B
// CHECK: :[[@LINE-1]]:16: implicit `base()` chain: base class `B` has no parameterless constructor; the chain must be explicit (§12.8)
end NoCtor;

// Spelling 2 — a constructor that writes neither `base(...)` nor `this(...)`.
// §12.8's sentence is about the CHAIN, not about the absence of a
// constructor, so the diagnostic lands on the constructor that owes one.
class CtorNoChain : B
  public k: int := 0;
  public constructor()
  // CHECK: :[[@LINE-1]]:3: implicit `base()` chain: base class `B` has no parameterless constructor; the chain must be explicit (§12.8)
    k := 1;
  end constructor;
end CtorNoChain;

// Spelling 3 — a §22.10 primary-constructor class whose inheritance clause
// names the base but supplies NO argument list. §22.10 makes a clause WITH
// arguments the base call ("This replaces an explicit `base(...)` call"); a
// bare clause writes none, so §12.8 inserts one.
class BarePrimary(v: int) : B
// CHECK: :[[@LINE-1]]:29: implicit `base()` chain: base class `B` has no parameterless constructor; the chain must be explicit (§12.8)
  public method Go() Show(); end Go;
end BarePrimary;

// Spelling 4 — the EXPLICIT zero-argument `base()`, over a base with no
// §22.10 primary parameter list at all. #4519 closed the explicit spelling
// through the §22.5 binder over the base's primary list, which this base does
// not have, so the call stayed silent; the SHARED predicate is what closes it.
// The remedy differs — an explicit `base()` cannot be made explicit — so the
// text differs in its second half and is pinned separately here.
class ExplicitZero : Bx
  public constructor()
    base();
    // CHECK: :[[@LINE-1]]:5: `base()` chain: base class `Bx` has no parameterless constructor; supply its arguments (§12.8)
  end constructor;
end ExplicitZero;

class C
  public static method Main(): int
    return 0;
  end Main;
end C;
