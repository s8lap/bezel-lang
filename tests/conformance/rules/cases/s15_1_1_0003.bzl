//! case: s15_1_1_0003
//! doc: spec
//! section: 15.1.1
//! title: string fails the class-or-interface restriction of an owning inline array at instantiation
//! rule: When T is a generic type parameter, the class-or-interface
//!   restriction on `[N owned]T?` is checked at instantiation, so
//!   instantiating with T = `string` is a compile error at the instantiation
//!   site (§15.1.1).
//! cites: 15.1.1, 14.2
//! expect: reject
//! diagnostic: class or interface
//! issue: 4367
module Conf;

public class Pool<T> where T: class
  private _slots: [4 owned]T?;

  public constructor()
  end constructor;
end Pool;

public class Holder
  private _p: Pool<string>?;

  public constructor()
  end constructor;
end Holder;
