//! case: s22_15_0810
//! doc: spec
//! section: 22.15
//! title: an impure call in the guard of a case expression nested in a case arm of a pure method is rejected
//! rule: A call — method, operator, accessor read, or constructor in the admitted positions — is permitted iff the member declared on the receiver's static type (or the statically resolved member, for statics and extensions) is `pure`-declared or pure-classified (below).
//! cites: 22.15
//! expect: reject
//! diagnostic: pure
//! issue: 8134
//! notes: twin: the same program with Helper.Ok declared pure compiles (verified in scratch).
module Conf;

public class Helper
  public static method Ok(): bool
    Console.WriteLine("side effect");
    return true;
  end Ok;
end Helper;

public class Program
  public static pure method Run(x: int, y: int): int
    return (case x of 0 => (case y of 1 when Helper.Ok() => 5, _ => 6), _ => 0);
  end Run;
  public static method Main()
    Console.WriteLine(Run(0, 1));
  end Main;
end Program;
