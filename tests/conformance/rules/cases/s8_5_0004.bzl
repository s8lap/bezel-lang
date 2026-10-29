//! case: s8_5_0004
//! doc: spec
//! section: 8.5
//! title: an Object holding a StringBuilder renders its text until a closure reassigns the Object
//! rule: `ToString` is invoked via virtual dispatch for present values; a class
//!   instance held at an `Object` location renders through its own `ToString`.
//!   A captured `Object` variable assigned in a closure holds the new value
//!   afterwards.
//! cites: 8.5, 8.1
//! expect: run
//! output: sbtext
//!   later
module Conf;

public class Program
  public static method Main()
    var sb := new StringBuilder();
    _ := sb.Append("sbtext");
    var o: Object := sb;
    Console.WriteLine(o);
    var f := lambda() => do o := "later"; end;
    f();
    Console.WriteLine(o);
  end Main;
end Program;
