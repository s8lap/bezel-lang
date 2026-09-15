//! case: s22_13_0015
//! doc: spec
//! section: 22.13
//! title: a mutable static owning-inline-array field is admitted with a declaration-site warning
//! rule: A mutable static field of owning-inline-array type
//!   (`[N owned]T?` or `[N owned]T`) is admitted — with rule 4's
//!   declaration-site warning: the array's owned instances are reachable
//!   from every domain and its slots are mutable.
//! cites: 22.13, 22.12
//! expect: warn
//! diagnostic: one domain
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public class Registry
  private static _pool: [4 owned]Connection?;
end Registry;
