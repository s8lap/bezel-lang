//! case: s8_5_0006
//! doc: spec
//! section: 8.5
//! title: An Object holding a StringBuilder renders in a hole and Append(Object), then the new value
//! rule: `ToString` is invoked implicitly inside string interpolation via
//!   virtual dispatch for present values; `StringBuilder.Append(value: Object)`
//!   keeps `value.ToString()`'s text.
//! cites: 8.5, 18.5
//! expect: run
//! output: sbtext
//!   sbtext
//!   later
module Conf;

public class Program
  public static method Main()
    var sb := new StringBuilder();
    _ := sb.Append("sbtext");
    var o: Object := sb;
    Console.WriteLine($"{o}");
    var t := new StringBuilder();
    _ := t.Append(o);
    Console.WriteLine(t.ToString());
    o := "later";
    Console.WriteLine(o);
  end Main;
end Program;
