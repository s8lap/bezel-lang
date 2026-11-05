//! case: l3_3_1108
//! doc: stdlib
//! section: 3.3
//! title: a copy of a span assigned to an outer span outlives the loop body
//! rule: the span may sit in fields, cross `await`, and outlive any scope, because the `Source`
//!   reference itself keeps the string alive
//! cites: 4.6
//! expect: run
//! output: hello
//! issue: 8162
module Conf;

public class Program
  public static method Main()
    var keep := new ReadOnlyStringSpan("", 0, 0);
    for var i in 0 .. 99 do
      var w := "hello" + i.ToString();
      var sp := new ReadOnlyStringSpan(w, 0, 5);
      var sp2 := sp;
      keep := sp2;
    end for;
    Console.WriteLine(keep.ToString());
  end Main;
end Program;
