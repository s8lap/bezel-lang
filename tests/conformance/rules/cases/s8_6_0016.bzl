//! case: s8_6_0016
//! doc: spec
//! section: 8.6
//! title: a double used as the fill of the sized Array<Object> constructor is boxed and renders shortest
//! rule: When a value type is used where `Object` (or an interface implemented
//!   by the value type) is expected, the value is **boxed**: a
//!   heap-allocated wrapper holds the value.
//! cites: 8.6
//! expect: run
//! issue: 8037
//! output: [1.5][1.5]
module Conf;

public class Program
  public static method Main()
    using var sb := new StringBuilder();
    using var a: Array<Object> := new Array<Object>(2, 1.5);
    sb.AppendFormat("[{0}][{1}]", a);
    Console.WriteLine(sb.ToString());
  end Main;
end Program;
