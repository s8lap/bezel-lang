//! case: s22_12_0049
//! doc: spec
//! section: 22.12
//! title: a static readonly owning inline array is admitted and its slots may be read
//! rule: An owning-inline-array-typed `static readonly` field takes the default
//!   warning and *is* disposed at exit by §22.13's owning-inline-array clause;
//!   it is the one admitted static of owning-inline-array type (a mutable one
//!   is rule 2's closing error).
//! cites: 22.12, 15.1.1
//! expect: accept
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public class Registry
  public static readonly Pool: [2 owned]Connection?;

  public static method IsEmpty(): bool
    return Registry.Pool[0] = null;
  end IsEmpty;
end Registry;
