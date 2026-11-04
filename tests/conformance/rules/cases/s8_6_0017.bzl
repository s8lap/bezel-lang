//! case: s8_6_0017
//! doc: spec
//! section: 8.6
//! title: a uint above int.MaxValue stored by fill, indexer and insert is boxed and keeps its value
//! rule: When a value type is used where `Object` (or an interface implemented
//!   by the value type) is expected, the value is **boxed**: a
//!   heap-allocated wrapper holds the value.
//! cites: 8.6
//! expect: run
//! issue: 8037
//! output: [4000000000][4000000000]
module Conf;

public class Program
  public static method Main()
    using var sb := new StringBuilder();
    var u: uint := 4000000000u;
    using var a: Array<Object> := new Array<Object>(1, u);
    a[0] := u;
    a.Insert(0, u);
    sb.AppendFormat("[{0}][{1}]", a);
    Console.WriteLine(sb.ToString());
  end Main;
end Program;
