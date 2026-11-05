//! case: s26_7_1104
//! doc: spec
//! section: 26.7
//! title: a string extension returning this from a for loop hands back the receiver
//! rule: The receiver is always a borrow; extensions cannot store `this`, return it as `owned`,
//!   or dispose it.
//! cites: 22.5
//! expect: run
//! output: 293
//! issue: 7841
module Conf;

public record Named(Tag: string, N: int);

public class Holder
  public V: string := "";
end Holder;

public class Make
  public static method Str(n: int): string => "s" + n.ToString();
end Make;

public extension string
  public method Id(): string
    for var k in 0 .. 2 do
      if k = 1 then
        return this;
      end if;
    end for;
    return "x";
  end Id;
end;

public class Program
  public static method Main()
    var t := 0;
    using var l := new Array<string>();
    var h := new Holder();
    for var i in 0 .. 99 do
      l.Add(Make.Str(i).Id());
      h.V := Make.Str(i).Id();
    end for;
    for var s in l do
      t := t + s.Length;
    end for;
    t := t + h.V.Length;
    Console.WriteLine(t);
  end Main;
end Program;
