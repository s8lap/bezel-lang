//! case: s14_2_0017
//! doc: spec
//! section: 14.2
//! title: int satisfies where T: hashable
//! rule: `where T: hashable` admits types whose hashable derived type property
//!   holds — every type except a non-auto-hashable record (or choice) with no
//!   user equality (§14.2, §3.6).
//! cites: 14.2, 3.6
//! expect: accept
module Conf;

public class Box<T> where T: hashable
  public constructor()
  end constructor;
end Box;

public class Holder
  private _b: Box<int>?;

  public constructor()
  end constructor;
end Holder;
