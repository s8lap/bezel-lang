//! case: s14_2_0007
//! doc: spec
//! section: 14.2
//! title: a class implementing the named interface satisfies where T: IFoo
//! rule: `where T: IFoo` admits non-nullable types conforming to `IFoo`:
//!   classes implementing it, directly or by inheritance (§14.2).
//! cites: 14.2
//! expect: accept
//! issue: 4367
module Conf;

public interface IFoo
  method Ping();
end IFoo;

public class Box<T> where T: IFoo
  public constructor()
  end constructor;
end Box;

public class Impl: IFoo
  public constructor()
  end constructor;

  public method Ping() => Console.WriteLine("ping");
end Impl;

public class Holder
  private _b: Box<Impl>?;

  public constructor()
  end constructor;
end Holder;
