//! case: s14_2_0017
//! doc: spec
//! section: 14.2
//! title: int satisfies where T: hashable
//! rule: Types whose **hashable** derived type property (§3.6) holds —
//!   consistent `Equals`/`GetHashCode`, usable as hash-container keys and
//!   elements — decided per instantiation by §3.6's computation.
//! cites: 14.2, 3.6
//! expect: accept
//! issue: 4367
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
