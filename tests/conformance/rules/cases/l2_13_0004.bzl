//! case: l2_13_0004
//! doc: stdlib
//! section: 2.13
//! title: a generic instantiation's FullName recurses through its arguments
//! rule: A generic instantiation renders the head followed by "<", its
//!   arguments joined by ", ", and ">", each argument rendered recursively
//!   in the same property's form (the document's own worked example).
//! cites: 2.13
//! expect: run
//! stdout: System.Collections.Generic.Dictionary<string, System.Collections.Generic.Array<int>>
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(typeof(Dictionary<string, Array<int>>).FullName);
  end Main;
end Program;
