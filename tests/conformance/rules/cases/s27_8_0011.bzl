//! case: s27_8_0011
//! doc: spec
//! section: 27.8
//! title: a nested generic alias argument on the base parameter is overridden through a base receiver
//! rule: Type aliases — generic or not — see through to the aliased type for identity, equality, method dispatch, and overload resolution.
//! cites: 27.8, 12.1
//! issue: 6323
//! notes: pins #6323 AND, independently, #6321. Both legs re-measured with
//!   PROVABLY FRESH binaries (`bezelc --version` embedding the tree's HEAD,
//!   linked after the newest src/ mtime), because an earlier measurement of
//!   this claim was taken against a stale compiler and had to be redone.
//!   Reverting only 92135ec1 (#6321), #6323 present: REJECTS with "override
//!   of `M` does not match the base member's parameter types (§12.1)".
//!   Reverting only 9013a150 (#6323): prints `base|derived` (the non-author
//!   review's leg). So #6321 alone breaks this row and #6323 alone breaks it
//!   too; neither is redundant for `Id2<Id<X>>`.
//! expect: run
//! output: derived
//!   derived
module Conf;

type Id<T> := T;
type Id2<T> := Id<T>;

public class B<X>
  public constructor() end constructor;
  public virtual method M(x: Id2<Id<X>>): string => "base";
end B;

public class E<Q>: B<Q>
  public constructor() end constructor;
  public override method M(x: Q): string => "derived";
end E;

public class M
  public static method Main(): int
    var e := new E<int>();
    var b: B<int> := e;
    Console.WriteLine(b.M(5));
    Console.WriteLine(e.M(5));
    return 0;
  end Main;
end M;
