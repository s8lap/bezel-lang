//! case: s22_5_0014
//! doc: spec
//! section: 22.5
//! title: a call through a constraint head that leaves a required parameter unbound is rejected
//! rule: **Every parameter that declares no default must be bound**, positionally
//!   or by name; leaving one unbound is the opposite disposition from
//!   binding one twice — it makes the candidate **inapplicable**, so at an
//!   overloaded name an applicable sibling still resolves and the error is
//!   reported only where no candidate survives.
//! cites: 22.5
//! expect: reject
//! diagnostic: argument
module Conf;

public class Program
  public static method G<T>(x: T): int where T: IList<int>
    x.Insert(0);
    return 0;
  end G;

  public static method Main(): int
    var a: Array<int> := [1, 2, 3];
    Console.WriteLine(Program.G(a));
    return 0;
  end Main;
end Program;
