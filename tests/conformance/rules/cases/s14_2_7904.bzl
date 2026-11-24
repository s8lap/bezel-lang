//! case: s14_2_7904
//! doc: spec
//! section: 14.2
//! title: a where T: value bound admits the stdlib value record TimeSpan at an instantiation
//! rule: | `where T: value` | Non-nullable value types — records, enums, choices
//! cites: 14.2
//! expect: run
//! stdout: 7
module Conf;
import System;

public class Box<T> where T: value
  public method N(): int => 7;
end Box;

public class Program
  public static method Main(): int
    using var b := new Box<TimeSpan>();
    Console.WriteLine(b.N());
    return 0;
  end Main;
end Program;
