//! case: s35_10_0001
//! doc: spec
//! section: 35.10
//! title: default(C) is a compile error for every choice type
//! rule: default(C) is a compile error for every choice type — no choice is
//!   defaultable: a sum yields no distinguished zero.
//! expect: reject
//! diagnostic: defaultable
module Conf;

public choice Status
  Active,
  Inactive;
end Status;

public class Program
  public static method Main()
    var s := default(Status);   // ERROR: no choice type is defaultable
  end Main;
end Program;
