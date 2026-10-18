//! case: s27_8_0012
//! doc: spec
//! section: 27.8
//! title: an override and its base both spelled through an alias of an alias dispatch through a base receiver
//! rule: Type aliases — generic or not — see through to the aliased type for identity, equality, method dispatch, and overload resolution.
//! cites: 27.8, 12.1
//! issue: 6323
//! notes: pins #6323 alone. Reverting only 9013a150 (#6323) prints
//!   `base|derived`; reverting only #6321 (92135ec1), with a provably fresh
//!   binary, leaves this row green (`derived/derived`) -- both sides are
//!   spelled through `Id2` and keep the same stale `T`, which is the case
//!   #6323's body names as newly broken. So this row does not require #6321;
//!   s27_8_0008 and s27_8_0011 do.
//! expect: run
//! output: derived
//!   derived
module Conf;

type Id<T> := T;
type Id2<T> := Id<T>;

public class B<X>
  public constructor() end constructor;
  public virtual method M(x: Id2<X>): string => "base";
end B;

public class E<Q>: B<Q>
  public constructor() end constructor;
  public override method M(x: Id2<Q>): string => "derived";
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
