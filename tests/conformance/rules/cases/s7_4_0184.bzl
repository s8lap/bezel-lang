//! case: s7_4_0184
//! doc: spec
//! section: 7.4
//! title: a null test of a static readonly field inside a static field initializer establishes no fact
//! rule: The rule does not apply inside the declaring type's constructors, field initializers, static
//!   constructor, or static field initializers (the assignment window)
//! cites: 22.8, 12.7
//! expect: reject
//! diagnostic: narrow
//! issue: 8099
module Conf;

public class H
  public static readonly S: string? := "abc";
  public static readonly T: int := (if H.S <> null then H.S.Length else 0);
end H;

public class Program
  public static method Main()
    Console.WriteLine(H.T);
  end Main;
end Program;
