//! case: l3_3_1101
//! doc: stdlib
//! section: 3.3
//! title: ToString of a full-length span built from a local string is valid after the local's method returns
//! rule: the span may sit in fields, cross `await`, and outlive any scope, because the `Source`
//!   reference itself keeps the string alive
//! cites: 4.6
//! expect: run
//! output: hello5
//!   690
//! issue: 8162
module Conf;

public class Program
  public static method F(i: int): string
    var w := "hello" + i.ToString();
    var sp := new ReadOnlyStringSpan(w, 0, w.Length);
    return sp.ToString();
  end F;

  public static method Main()
    var t := 0;
    for var i in 0 .. 99 do
      var s := F(i);
      t := t + s.Length;
      if i = 5 then
        Console.WriteLine(s);
      end if;
    end for;
    Console.WriteLine(t);
  end Main;
end Program;
