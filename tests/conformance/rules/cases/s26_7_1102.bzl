//! case: s26_7_1102
//! doc: spec
//! section: 26.7
//! title: a string extension interpolating this into a record field yields the receiver's text
//! rule: The receiver is always a borrow; extensions cannot store `this`, return it as `owned`,
//!   or dispose it.
//! cites: 22.5
//! expect: run
//! output: 290
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
  public method W(): Named => new Named($"{this}", 1);
end;

public class Program
  public static method Main()
    var t := 0;
    using var l := new Array<Named>();
    for var i in 0 .. 99 do
      l.Add(Make.Str(i).W());
    end for;
    for var n in l do
      t := t + n.Tag.Length;
    end for;
    Console.WriteLine(t);
  end Main;
end Program;
