//! case: s8_6_0020
//! doc: spec
//! section: 8.6
//! title: an already-boxed Object inserted into an Array<Object> keeps its value
//! rule: When a value type is used where `Object` (or an interface implemented
//!   by the value type) is expected, the value is **boxed**: a
//!   heap-allocated wrapper holds the value.
//! cites: 8.6
//! expect: run
//! issue: 8037
//! output: [5][x]
module Conf;

public class Program
  public static method Main()
    using var sb := new StringBuilder();
    var o: Object := 5;
    using var a: Array<Object> := new Array<Object>();
    a.Add("x");
    a.Insert(0, o);
    sb.AppendFormat("[{0}][{1}]", a);
    Console.WriteLine(sb.ToString());
  end Main;
end Program;
