//! case: s27_8_0012
//! doc: spec
//! section: 27.8
//! title: an override and its base both spelled through an alias of an alias dispatch through a base receiver
//! rule: Type aliases — generic or not — see through to the aliased type for identity, equality, method dispatch, and overload resolution.
//! cites: 27.8, 12.1
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
