//! case: s14_2_0016
//! doc: spec
//! section: 14.2
//! title: an interface type fails where T: sendable
//! rule: `where T: sendable` is decided by the §30.14 computation; interface
//!   types are never sendable (§14.2, §30.14).
//! cites: 14.2, 30.14
//! expect: reject
//! diagnostic: sendable
module Conf;

public interface IFoo
  method Ping();
end IFoo;

public class Box<T> where T: sendable
  public constructor()
  end constructor;
end Box;

public class Holder
  private _b: Box<IFoo>?;

  public constructor()
  end constructor;
end Holder;
