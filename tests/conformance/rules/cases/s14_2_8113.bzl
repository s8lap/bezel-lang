//! case: s14_2_8113
//! doc: spec
//! section: 14.2
//! title: an alias of a stdlib record does not satisfy a class bound at an instantiation
//!  site
//! rule: `where T: class` | Non-nullable reference types only
//! cites: 14.2, 27.8
//! expect: reject
//! diagnostic: class
//! notes: Pins #9033's class arm at the instantiation site (the call site is s14_2_8111).
module Conf;
type TS := TimeSpan;
public class Box<T> where T: class
  public method N(): int => 7;
end Box;
public class P
  public static method Main(): int
    var b := new Box<TS>();
    Console.WriteLine(b.N());
    return 0;
  end Main;
end P;
