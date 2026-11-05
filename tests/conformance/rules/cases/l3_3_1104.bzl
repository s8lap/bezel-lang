//! case: l3_3_1104
//! doc: stdlib
//! section: 3.3
//! title: a Slice of a Slice returned from a method is valid after the source local's scope ends
//! rule: the span may sit in fields, cross `await`, and outlive any scope, because the `Source`
//!   reference itself keeps the string alive
//! cites: 4.6
//! expect: run
//! output: el
//!   200
//! issue: 8162
module Conf;

public class Program
  public static method F(i: int): ReadOnlyStringSpan
    var w := "hello" + i.ToString();
    var sp := new ReadOnlyStringSpan(w, 0, 5);
    var d := sp.Slice(1, 3);
    var e := d.Slice(0, 2);
    return e;
  end F;

  public static method Main()
    var t := 0;
    for var i in 0 .. 99 do
      var s := F(i);
      t := t + s.Length;
      if i = 5 then
        Console.WriteLine(s.ToString());
      end if;
    end for;
    Console.WriteLine(t);
  end Main;
end Program;
