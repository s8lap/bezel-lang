//! case: s14_2_0018
//! doc: spec
//! section: 14.2
//! title: a record with an explicit field and no user equality fails where T: hashable
//! rule: `hashable` excludes a record that is neither auto-hashable nor
//!   declares user equality; an explicit field declaration makes a body
//!   record not auto-hashable (§14.2, §3.6, §9.2).
//! cites: 14.2, 3.6, 9.2
//! expect: reject
//! diagnostic: hashable
//! issue: 4367
module Conf;

public record OddRecord
  private _x: int;

  public constructor(x: int)
    _x := x;
  end constructor;
end OddRecord;

public class Box<T> where T: hashable
  public constructor()
  end constructor;
end Box;

public class Holder
  private _b: Box<OddRecord>?;

  public constructor()
  end constructor;
end Holder;
