//! case: s14_2_0015
//! doc: spec
//! section: 14.2
//! title: int satisfies where T: sendable
//! rule: `where T: sendable` admits types whose sendable derived type property
//!   holds; all primitives are sendable (§14.2, §30.14).
//! cites: 14.2, 30.14
//! expect: accept
module Conf;

public class Box<T> where T: sendable
  public constructor()
  end constructor;
end Box;

public class Holder
  private _b: Box<int>?;

  public constructor()
  end constructor;
end Holder;
