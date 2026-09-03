// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Issue #3769 / #3777 — the row that pins `CtorSelect::ChainSite`, REVERSED.
//
// It used to pin the OPPOSITE verdict, and the reason is worth keeping. The two
// LOWERING resolvers ownership must agree with did not agree with each other: a
// `new T(args)` site lowered through codegen's `ctorForArgs`, which tries an
// explicit constructor whose formals the arguments can FILL with §22.5 defaults
// admitted (fix #3082), while a `this(...)` chain lowered through
// `emitChainCall`, which used `ctorByArity` — EXACT arity, no defaults — and
// then fell through to the §22.10 primary arm. This class has no primary, so
// the chain below lowered to NOTHING (measured on 89eb914: the plain twin of
// this program printed `0/0`). Classifying `x` as transferred into a
// constructor that never runs would have suppressed the §28.3 auto-drop and
// LEAKED the object (measured: 800 B / 50 objects), so ownership deliberately
// declined — and this file asserted the ACCEPT that the auto-drop warning
// accompanies.
//
// fix #3777 makes `emitChainCall` select with `ctorForArgs` and materialise the
// omitted §22.5 tail, so `B.constructor(owned a: Res, b: int := 3)` really is
// the constructor this chain calls, and it really does adopt the argument into
// `_f`. The position IS an owned sink now, so §22.3 requires the call site to
// announce the transfer — and leaving it unmarked, with the auto-drop still
// firing, is a DOUBLE FREE rather than a leak.
//
// The marked spelling that this rejection points at is the accepted twin,
// `defaulted_ctor_arity_3769__chain_to_explicit_sibling_marked_accepted.bzl`.

module M;

class Res
  private _id: int;
  public constructor(id: int) _id := id; end constructor;
  public method Id(): int => _id;
end Res;

class B
  private owned _f: Res;
  private _n: int;
  public constructor(owned a: Res, b: int := 3)
    _f := move a;
    _n := b;
  end constructor;
  public constructor(owned x: Res, y: bool)
    // CHECK: this call consumes `x`
    // CHECK-SAME: §22.3
    this(x);
  end constructor;
  public method Id(): int
    return _n;
  end Id;
end B;
