//! case: s3_6_0009
//! doc: spec
//! section: 3.6
//! title: a value-like choice outside auto-hashable only through its declared equality is hashable
//! rule: hashable ... every type except ... (b) a choice that is not
//!   value-like, or is value-like and neither auto-hashable nor declares user
//!   equality (§35.7 — ... a value-like choice outside auto-hashable through
//!   its own declared equality — `Equals(other: C)` with `GetHashCode`, with
//!   or without a homogeneous `=`/`<>` overload beside them — is hashable on
//!   the strength of those members)
//! cites: 3.6, 35.7, 14.2
//! expect: accept
module Conf;

public class Item
  public constructor()
  end constructor;
end Item;

public choice Ref
  Some(it: int),
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
