//! case: s14_2_8116
//! doc: spec
//! section: 14.2
//! title: an alias of a stdlib record satisfies a value bound at an instantiation site
//! rule: `where T: value` | Non-nullable value types — records, enums, choices
//! cites: 14.2, 27.8
//! expect: run
//! output: 7
//! notes: Pins #9033's value arm at the instantiation site; admitting twin of s14_2_8113.
module Conf;
type TS := TimeSpan;
public class Box<T> where T: value
  public method N(): int => 7;
end Box;
public class P
  public static method Main(): int
    var b := new Box<TS>();
    Console.WriteLine(b.N());
    return 0;
  end Main;
end P;
