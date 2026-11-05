//! case: l3_3_1105
//! doc: stdlib
//! section: 3.3
//! title: a Slice passed to a method that stores it in a field outlives the loop body
//! rule: the span may sit in fields, cross `await`, and outlive any scope, because the `Source`
//!   reference itself keeps the string alive
//! cites: 4.6
//! expect: run
//! output: ell
//! issue: 8162
module Conf;

public class Box
  public Keep: ReadOnlyStringSpan;

  public constructor()
    this.Keep := new ReadOnlyStringSpan("", 0, 0);
  end constructor;

  public method Store(s: ReadOnlyStringSpan)
    this.Keep := s;
  end Store;
end Box;

public class Program
  public static method Main()
    var bx := new Box();
    for var i in 0 .. 99 do
      var w := "hello" + i.ToString();
      var sp := new ReadOnlyStringSpan(w, 0, 5);
      var d := sp.Slice(1, 3);
      bx.Store(d);
    end for;
    Console.WriteLine(bx.Keep.ToString());
  end Main;
end Program;
