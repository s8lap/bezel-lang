//! case: l3_3_1107
//! doc: stdlib
//! section: 3.3
//! title: ToString of a full-length span assigned to an outer string outlives the loop body
//! rule: the span may sit in fields, cross `await`, and outlive any scope, because the `Source`
//!   reference itself keeps the string alive
//! cites: 4.6
//! expect: run
//! output: hello99
//! issue: 8162
module Conf;

public class Program
  public static method Main()
    var o := "";
    for var i in 0 .. 99 do
      var w := "hello" + i.ToString();
      var sp := new ReadOnlyStringSpan(w, 0, w.Length);
      o := sp.ToString();
    end for;
    Console.WriteLine(o);
  end Main;
end Program;
