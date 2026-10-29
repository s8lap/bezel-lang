//! case: s28_3_0301
//! doc: spec
//! section: 28.3
//! title: owned StringBuilder temporaries from interface and generic calls live to the end of their expression
//! rule: Such a temporary is disposed at the **end of the full expression** in which it is produced, after every use of it within that expression has completed ... every `this`-returning call on it is a borrow of that temporary
//! cites: 28.3, 28.6
//! expect: run
//! stdout: 1650
module Conf;

interface IMk
  method Make(): owned StringBuilder;
end IMk;

class MkA: IMk
  public method Make(): owned StringBuilder
    return new StringBuilder("A");
  end Make;
end MkA;

class Box<T>
  public static method Id(x: owned T): owned T
    return x;
  end Id;
end Box;

public class Program
  public static method Gen<T>(x: owned T): owned T
    return x;
  end Gen;

  public static method Main()
    var total := 0;
    using var a := new MkA();
    var i1: IMk := a;
    for var i in 0..<150 do
      total += i1.Make().Append("q").ToString().Length;
      total += Gen<StringBuilder>(new StringBuilder("gen")).Length;
      total += Gen<StringBuilder>(new StringBuilder("gen")).Append("1").ToString().Length;
      total += Box<StringBuilder>.Id(new StringBuilder("bx")).Length;
    end for;
    Console.WriteLine(total);
  end Main;
end Program;
