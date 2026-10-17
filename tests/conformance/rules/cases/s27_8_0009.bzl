//! case: s27_8_0009
//! doc: spec
//! section: 27.8
//! title: an override whose parameter is spelled through a generic alias dispatches through a base receiver
//! rule: Type aliases — generic or not — see through to the aliased type for identity, equality, method dispatch, and overload resolution.
//! cites: 27.8, 12.1
//! expect: run
//! output: derived
//!   derived
module Conf;

type Id<T> := T;

public class B<X>
  public constructor() end constructor;
  public virtual method M(x: Id<X>): string => "base";
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
