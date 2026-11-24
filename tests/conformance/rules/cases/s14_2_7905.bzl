//! case: s14_2_7905
//! doc: spec
//! section: 14.2
//! title: a where T: value bound rejects string, a reference type
//! rule: | `where T: value` | Non-nullable value types — records, enums, choices
//! cites: 14.2, 3.1
//! expect: reject
//! diagnostic: value
module Conf;
import System;

public class Box<T> where T: value
  public method N(): int => 7;
end Box;

public class Program
  public static method Main(): int
    using var b := new Box<string>();
    Console.WriteLine(b.N());
    return 0;
  end Main;
end Program;
