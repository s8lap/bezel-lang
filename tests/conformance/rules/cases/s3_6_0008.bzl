//! case: s3_6_0008
//! doc: spec
//! section: 3.6
//! title: a borrow-bearing choice declaring user equality is still not hashable
//! rule: hashable ... every type except ... (b) a choice that is not
//!   value-like, or is value-like and neither auto-hashable nor declares user
//!   equality (§35.7 — owning and borrow-bearing choices are not hashable,
//!   user equality declared or not
//! cites: 3.6, 35.7, 14.2
//! expect: reject
//! diagnostic: hashable
module Conf;

public class Item
  public constructor()
  end constructor;
end Item;

public choice Ref
  Some(it: Item),
  None;

  public static method operator =(a: Ref, b: Ref): bool => true;
  public static method operator <>(a: Ref, b: Ref): bool => false;
  public method Equals(other: Ref): bool => true;
  public method GetHashCode(): int => 0;
end Ref;

public class Bag<T> where T: hashable
  public constructor()
  end constructor;
end Bag;

public class Holder
  public static method Make(): int
    using var b := new Bag<Ref>();
    return 1;
  end Make;
end Holder;
