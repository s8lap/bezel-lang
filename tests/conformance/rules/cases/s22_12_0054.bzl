//! case: s22_12_0054
//! doc: spec
//! section: 22.12
//! title: a mutable static owning inline array field with an initializer is rejected
//! rule: it is the one admitted static of owning-inline-array type (a mutable one
//!   is rule 2's closing error)
//! cites: 22.12, 15.1.1
//! expect: reject
//! diagnostic: static
module Conf;

public class Res
  public Name: string;
  public constructor(n: string)
    this.Name := n;
  end constructor;
end Res;

public class Registry
  public static Pool: [2 owned]Res? := [new Res("x"), new Res("y")];
end Registry;
