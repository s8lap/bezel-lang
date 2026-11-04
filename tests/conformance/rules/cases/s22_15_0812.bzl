//! case: s22_15_0812
//! doc: spec
//! section: 22.15
//! title: an impure call in a case expression in a throw constructor argument in a pure method is rejected
//! rule: A call — method, operator, accessor read, or constructor in the admitted positions — is permitted iff the member declared on the receiver's static type (or the statically resolved member, for statics and extensions) is `pure`-declared or pure-classified (below).
//! cites: 22.15
//! expect: reject
//! diagnostic: pure
//! issue: 8134
//! notes: twin: the same program with Helper.Msg declared pure compiles (verified in scratch).
module Conf;

public class Helper
  public static method Msg(): string
    Console.WriteLine("side effect");
    return "m";
  end Msg;
end Helper;

public class Program
  public static pure method Run(x: int): int
    if x < 0 then
      throw new Exception((case x of -1 => Helper.Msg(), _ => "z"));
    end;
    return x;
  end Run;
  public static method Main()
    try
      Console.WriteLine(Run(-1));
    catch var e: Exception => Console.WriteLine("caught " + e.Message);
    end;
  end Main;
end Program;
