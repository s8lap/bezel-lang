//! case: s8_6_0018
//! doc: spec
//! section: 8.6
//! title: an int stored through an IList<Object> indexer is boxed
//! rule: When a value type is used where `Object` (or an interface implemented
//!   by the value type) is expected, the value is **boxed**: a
//!   heap-allocated wrapper holds the value.
//! cites: 8.6
//! expect: run
//! issue: 8037
//! output: [9]
module Conf;

public class G
  public static method Put(l: IList<Object>, v: int)
    l[0] := v;
  end Put;
end G;

public class Program
  public static method Main()
    using var sb := new StringBuilder();
    using var a: Array<Object> := new Array<Object>();
    a.Add("x");
    G.Put(a, 9);
    sb.AppendFormat("[{0}]", a);
    Console.WriteLine(sb.ToString());
  end Main;
end Program;
