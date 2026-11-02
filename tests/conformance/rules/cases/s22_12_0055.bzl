//! case: s22_12_0055
//! doc: spec
//! section: 22.12
//! title: a mutable static owning inline array field in a static class is rejected
//! rule: it is the one admitted static of owning-inline-array type (a mutable one
//!   is rule 2's closing error)
//! cites: 22.12, 15.1.1, 11.7
//! expect: reject
//! diagnostic: static
module Conf;

public class Res
  public Name: string;
  public constructor(n: string)
    this.Name := n;
  end constructor;
end Res;

public static class Globals
  public static Arr: [2 owned]Res?;
end Globals;
