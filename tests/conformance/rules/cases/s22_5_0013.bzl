//! case: s22_5_0013
//! doc: spec
//! section: 22.5
//! title: a call through a constraint head that supplies two arguments for one parameter is rejected
//! rule: Each supplied argument binds **exactly one** parameter and each
//!   parameter is bound by **at most one** supplied argument: naming a
//!   parameter already bound positionally ... or naming the same parameter
//!   twice, is a compile error at the call naming that parameter.
//! cites: 22.5
//! expect: reject
//! diagnostic: argument
module Conf;

public class Program
  public static method G<T>(x: T): int where T: IList<int>
    x.Add(7, 8);
    return 0;
  end G;

  public static method Main(): int
    var a: Array<int> := [1, 2, 3];
    Console.WriteLine(Program.G(a));
    return 0;
  end Main;
end Program;
