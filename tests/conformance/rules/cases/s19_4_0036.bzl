//! case: s19_4_0036
//! doc: spec
//! section: 19.4
//! title: a subtype arm written before its supertype arm takes the subtype value
//! rule: Arms are evaluated in textual order.
//! cites: 19.4, 19.3
//! expect: run
//! output: K
//!   K
//!   obj
//!   null
module Conf;

public class K
end K;

public class D: K
end D;

public class Program
  public static method Classify(o: Object?): string => (case o of
    K      => "K",
    Object => "obj",
    _      => "null");

  public static method Main()
    Console.WriteLine(Classify(new K()));
    Console.WriteLine(Classify(new D()));
    Console.WriteLine(Classify("s"));
    Console.WriteLine(Classify(null));
  end Main;
end Program;
