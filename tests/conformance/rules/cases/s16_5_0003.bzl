//! case: s16_5_0003
//! doc: spec
//! section: 16.5
//! title: a counted for and IEnumerable<byte> drains of one byte range observe the same 201 values
//! rule: provided the observable behavior is identical
//! cites: 16.3
//! expect: run
//! output: 201
//!   201
//!   201
//! issue: 8637
module Conf;

public class Program
  private static method CountB(xs: IEnumerable<byte>): int
    var n := 0;
    for var x in xs do
      n++;
    end;
    return n;
  end CountB;

  private static method CountG<T>(xs: IEnumerable<T>): int
    var n := 0;
    for var x in xs do
      n++;
    end;
    return n;
  end CountG;

  public static method Main()
    var a: byte := 0;
    var b: byte := 200;
    var m := 0;
    for var x in a..b do
      m++;
    end;
    Console.WriteLine(m);
    Console.WriteLine(CountB(a..b));
    Console.WriteLine(CountG<byte>(a..b));
  end Main;
end Program;
