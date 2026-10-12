//! case: s3_3_0001
//! doc: spec
//! section: 3.3
//! title: a reference type is non-nullable by default
//! rule: A reference type is non-nullable by default.
//! expect: run
//! stdout: not null
module Contained;

public class Container
  public method GetString(): string
    return "not null";
  end GetString;
end Container;

public class Contained
  public static method Main()
    var c := new Container();
    var s := c.GetString();
    Console.WriteLine(s);
  end Main;
end Contained;
