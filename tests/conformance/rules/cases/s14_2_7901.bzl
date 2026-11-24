//! case: s14_2_7901
//! doc: spec
//! section: 14.2
//! title: an int type argument does not satisfy an IEnumerable<int> bound (Int32 lists no
//!   iteration interface)
//! rule: A primitive conforms to the interfaces its standard-library declaration lists
//! cites: 14.2
//! expect: reject
//! diagnostic: constraint
module Conf;
import System;

public class Box<S> where S: IEnumerable<int>
  public constructor() end constructor;
  public method N(): int => 1;
end Box;

public class Program
  public static method Main(): int
    using var b := new Box<int>();
    Console.WriteLine(b.N());
    return 0;
  end Main;
end Program;
