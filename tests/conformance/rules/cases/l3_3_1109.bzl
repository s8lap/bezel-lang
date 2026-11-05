//! case: l3_3_1109
//! doc: stdlib
//! section: 3.3
//! title: Slices added to an Array of spans in a loop are all valid after the loop
//! rule: the span may sit in fields, cross `await`, and outlive any scope, because the `Source`
//!   reference itself keeps the string alive
//! cites: 4.6
//! expect: run
//! output: 300
//! issue: 8162
module Conf;

public class Program
  public static method Main()
    var t := 0;
    using var l := new Array<ReadOnlyStringSpan>();
    for var i in 0 .. 99 do
      var w := "hello" + i.ToString();
      var sp := new ReadOnlyStringSpan(w, 0, 5);
      var d := sp.Slice(1, 3);
      l.Add(d);
    end for;
    for var s in l do
      t := t + s.ToString().Length;
    end for;
    Console.WriteLine(t);
  end Main;
end Program;
