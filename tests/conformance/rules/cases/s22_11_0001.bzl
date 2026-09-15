//! case: s22_11_0001
//! doc: spec
//! section: 22.11
//! title: a with expression over a class-typed source is rejected
//! rule: The object-initializer block does not exist: a `with` expression
//!   requires a record-typed source, and a class-typed source is a
//!   compile error.
//! cites: 22.11, 9.5
//! expect: reject
//! diagnostic: record
module Conf;

public class Widget
  public property Name: string;

  public constructor(name: string)
    Name := name;
  end constructor;
end Widget;

public class Program
  public static method Main()
    var w := new Widget("a") with(Name := "b");
  end Main;
end Program;
