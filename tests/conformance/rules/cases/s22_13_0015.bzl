//! case: s22_13_0015
//! doc: spec
//! section: 22.13
//! title: a mutable static owning-inline-array field is a compile error at the declaration
//! rule: **owning inline array** `[N owned]T?` / `[N owned]T` **field**
//!   | error (iii)-closed ... the tables' **error (iii)-closed** cells
//!   are compile errors under this rule's closing sentence
//! cites: 22.13, 22.12, 15.1.1
//! expect: reject
//! diagnostic: 22.12
//! issue: 5807
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public class Registry
  private static _pool: [4 owned]Connection?;
end Registry;
