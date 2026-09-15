//! case: s14_4_0003
//! doc: spec
//! section: 14.4
//! title: a value type element is rejected on OwningArray<T>
//! rule: Most standard-library owning types restrict their element type T to
//!   class and interface types; value types are rejected at the
//!   instantiation site (§14.4).
//! cites: 14.4
//! expect: reject
//! diagnostic: class
module Conf;

public class Holder
  private _p: OwningArray<int>?;

  public constructor()
  end constructor;
end Holder;
