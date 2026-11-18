//! case: s28_3_11_7202
//! doc: spec
//! section: 28.3.11
//! title: a string read from an unbound owned object temporary in a lambda
//!   survives its disposal
//! rule: is disposed at the **end of the full expression** in which it is
//!   produced, after every use of it within that expression has completed
//! cites: 28.3, 28.9, 22.2
//! expect: run
//! output: 1800
//!   boxboxbox
module Conf;

public class Box
  public Name: string := "bo" + "x";

  public method GetName(): string
    return Name;
  end GetName;
end Box;

public class P
  public static method MakeBox(): owned Box
    return new Box();
  end MakeBox;
end P;

public class Program
  public static method Main()
    var n := 0;
    var i := 0;
    var f := lambda() => P.MakeBox().Name;
    var g := lambda() => P.MakeBox().GetName();
    var h := lambda() => new Box().Name;
    while i < 200 do
      n := n + f().Length + g().Length + h().Length;
      i := i + 1;
    end while;
    Console.WriteLine(n);
    Console.WriteLine(f() + g() + h());
  end Main;
end Program;
