//! case: s26_5_9402
//! doc: spec
//! section: 26.5
//! title: a property TextWriter does not declare is rejected on Console.Out as a read
//! rule: Where neither rule 1 nor this search supplies the name, nothing supplies it: the access
//!   names no member and is a compile error at the name, the diagnostic naming the receiver's
//!   static type
//! cites: 26.5, stdlib 7.4
//! expect: reject
//! diagnostic: no member
module Conf;

public class Program
  public static method Main()
    var x := Console.Out.Bogus;
    Console.WriteLine("after");
  end Main;
end Program;
