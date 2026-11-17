//! case: s4_6_6237
//! doc: spec
//! section: 4.6
//! title: a record built in one method, returned and stored by the caller
//!   keeps its string field intact
//! rule: An immutable sequence of `char` (Unicode scalar values).
//! cites: 4.6, 9.7
//! expect: run
//! stdout: 12 x0,x1,x2,x3,x4,x5,x6,x7,x8,x9,x10,x11,
module Conf;

record Name(value: string, n: int);

class Sink
  public static readonly Items: Array<Name> := new Array<Name>();
end Sink;

public class Program

  static method Mk(i: int): Name
    var t := "x" + i.ToString();
    var n := new Name(t, 1);
    return n;
  end Mk;


  static method Once(i: int): int
    Sink.Items.Add(Mk(i));
    return 1;
  end Once;

  static method Dump(): string
    var joined := "";
    for var x in Sink.Items do
      joined := joined + x.value + ",";
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
