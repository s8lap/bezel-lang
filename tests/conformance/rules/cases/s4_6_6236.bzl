//! case: s4_6_6236
//! doc: spec
//! section: 4.6
//! title: a string field selected by an if-expression in a callee stays
//!   intact once stored
//! rule: An immutable sequence of `char` (Unicode scalar values).
//! cites: 4.6, 9.7
//! expect: run
//! stdout: 12 x0,x1,x2,x3,x4,x5,x6,x7,x8,x9,x10,x11,
module Conf;

record Name(value: string, n: int);

class Sink
  public static readonly Items: Array<Name> := new Array<Name>();
  public static readonly Texts: Array<string> := new Array<string>();
end Sink;

public class Program

  static method Pick(p: Name): string => (if p.n > 0 then p.value else "");


  static method Once(i: int): int
    var a := new Name("x" + i.ToString(), 1);
    Sink.Texts.Add(Pick(a));
    return a.n;
  end Once;

  static method Dump(): string
    var joined := "";
    for var x in Sink.Texts do
      joined := joined + x + ",";
    end;
    return joined;
  end Dump;

  public static method Main()
    var t := 0;
    for var i in 0..<12 do
      t := t + Once(i);
    end;
    Console.WriteLine($"{t} {Dump()}");
  end Main;
end Program;
