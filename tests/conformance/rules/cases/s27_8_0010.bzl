//! case: s27_8_0010
//! doc: spec
//! section: 27.8
//! title: a generic alias index parameter on the base is overridden by an indexer seen through a base receiver
//! rule: Type aliases — generic or not — see through to the aliased type for identity, equality, method dispatch, and overload resolution.
//! cites: 27.8, 12.1
//! expect: run
//! output: derived
//!   derived
module Conf;

type Id<T> := T;

public class B<X>
  public constructor() end constructor;
  public virtual property [i: Id<X>]: string
    get => "base";
  end;
end B;

public class E<Q>: B<Q>
  public constructor() end constructor;
  public override property [i: Q]: string
    get => "derived";
  end;
end E;

public class M
  public static method Main(): int
    var e := new E<int>();
    var b: B<int> := e;
    Console.WriteLine(b[5]);
    Console.WriteLine(e[5]);
    return 0;
  end Main;
end M;
