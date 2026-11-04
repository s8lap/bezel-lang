//! case: s8_6_0015
//! doc: spec
//! section: 8.6
//! title: an int used as the fill of the sized Array<Object> constructor is boxed
//! rule: When a value type is used where `Object` (or an interface implemented
//!   by the value type) is expected, the value is **boxed**: a
//!   heap-allocated wrapper holds the value.
//! cites: 8.6
//! expect: run
//! issue: 8037
//! output: [3][3]
module Conf;

public class Program
  public static method Main()
    using var sb := new StringBuilder();
    using var a: Array<Object> := new Array<Object>(2, 3);
    sb.AppendFormat("[{0}][{1}]", a);
    Console.WriteLine(sb.ToString());
  end Main;
end Program;
