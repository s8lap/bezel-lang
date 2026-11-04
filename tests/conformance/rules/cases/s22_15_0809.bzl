//! case: s22_15_0809
//! doc: spec
//! section: 22.15
//! title: an impure call nested in case expressions under ! inside a pure method is rejected
//! rule: A call — method, operator, accessor read, or constructor in the admitted positions — is permitted iff the member declared on the receiver's static type (or the statically resolved member, for statics and extensions) is `pure`-declared or pure-classified (below).
//! cites: 22.15
//! expect: reject
//! diagnostic: pure
//! issue: 8134
//! notes: twin s22_15_0803: the same program with Helper.Feed declared pure.
module Conf;

public class Helper
  public static method Feed(): string?
    Console.WriteLine("side effect");
    return "abc";
  end Feed;
end Helper;

public class Program
  public static pure method Run(x: int, y: int): int
    return (case x of 0 => (case y of 1 => Helper.Feed(), _ => "zz")!.Length, _ => 0);
  end Run;
  public static method Main()
    Console.WriteLine(Run(0, 1));
  end Main;
end Program;
