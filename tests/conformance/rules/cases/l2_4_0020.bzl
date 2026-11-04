//! case: l2_4_0020
//! doc: stdlib
//! section: 2.4
//! title: a call to Variables supplying an argument matches no declared form
//! rule: public static method Variables(): owned Dictionary<string, string>;
//! cites: 22.5
//! expect: reject
//! diagnostic: Variables
//! issue: 7949
//! notes: Variables declares no parameter; an argument has nothing to bind (spec 22.5).
module Conf;

public class Program
  public static method Main()
    using var v := Environment.Variables("x");
    Console.WriteLine(v.Count);
  end Main;
end Program;
