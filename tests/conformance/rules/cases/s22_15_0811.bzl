//! case: s22_15_0811
//! doc: spec
//! section: 22.15
//! title: an impure call under ! inside an arm of a case statement in a pure method is rejected
//! rule: A call — method, operator, accessor read, or constructor in the admitted positions — is permitted iff the member declared on the receiver's static type (or the statically resolved member, for statics and extensions) is `pure`-declared or pure-classified (below).
//! cites: 22.15
//! expect: reject
//! diagnostic: pure
//! issue: 8134
//! notes: twin: the same program with Helper.Feed declared pure compiles (verified in scratch).
module Conf;

public class Helper
  public static method Feed(): string?
    Console.WriteLine("side effect");
    return "abc";
  end Feed;
end Helper;

public class Program
  public static pure method Run(x: int): int
    var r := 0;
    case x of
      0 => do r := Helper.Feed()!.Length; end,
      _ => do r := 2; end,
    end;
    return r;
  end Run;
  public static method Main()
    Console.WriteLine(Run(0));
  end Main;
end Program;
