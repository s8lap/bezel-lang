//! case: s14_2_7903
//! doc: spec
//! section: 14.2
//! title: an Array<int> type argument satisfies an IEnumerable<int> bound
//! rule: `where T: IFoo` admits exactly the substitutions for which T conforms to `IFoo` per
//!   the rule above.
//! cites: 14.2
//! expect: run
//! stdout: 1
module Conf;
import System;

public class Box<S> where S: IEnumerable<int>
  public constructor() end constructor;
  public method N(): int => 1;
end Box;

public class Program
  public static method Main(): int
    using var b := new Box<Array<int>>();
    Console.WriteLine(b.N());
    return 0;
  end Main;
end Program;
