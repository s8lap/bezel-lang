//! case: s4_6_6238
//! doc: spec
//! section: 4.6
//! title: a record boxed into an Object local, read by a callee and then
//!   stored keeps its string field intact
//! rule: An immutable sequence of `char` (Unicode scalar values).
//! cites: 4.6, 8.6
//! expect: run
//! stdout: 12 x0,x1,x2,x3,x4,x5,x6,x7,x8,x9,x10,x11,
module Conf;

record Name(value: string, n: int);

class Sink
  public static readonly Objs: Array<Object?> := new Array<Object?>();
end Sink;

public class Program
  static method ReadO(o: Object?): int => (if o <> null then 1 else 0);

  static method Once(i: int): int
    var o: Object := new Name("x" + i.ToString(), 1);
    var k := ReadO(o);
    Sink.Objs.Add(o);
    return k;
  end Once;

  static method Dump(): string
    var joined := "";
    for var x in Sink.Objs do
      if x is Name then
        var v: Name := x;
        joined := joined + v.value + ",";
      end;
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
