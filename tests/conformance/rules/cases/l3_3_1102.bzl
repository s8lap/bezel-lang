//! case: l3_3_1102
//! doc: stdlib
//! section: 3.3
//! title: a Slice derived from a local span can be returned and read after the source local's scope ends
//! rule: the span may sit in fields, cross `await`, and outlive any scope, because the `Source`
//!   reference itself keeps the string alive
//! cites: 4.6
//! expect: run
//! output: ell
//!   300
//! issue: 8162
module Conf;

public class Program
  public static method F(i: int): ReadOnlyStringSpan
    var w := "hello" + i.ToString();
    var sp := new ReadOnlyStringSpan(w, 0, 5);
    var d := sp.Slice(1, 3);
    return d;
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
