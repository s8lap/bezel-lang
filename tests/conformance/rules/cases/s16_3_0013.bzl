//! case: s16_3_0013
//! doc: spec
//! section: 16.3
//! title: a range ending at the type's maximum drains through an IEnumerable and stops at the maximum
//! rule: `a..b` is inclusive on both ends ... yields exactly one value — the bound itself
//! cites: 16.3, 16.5
//! expect: run
//! output: 6
//!   1
//!   1
//!   1
//!   1
//! issue: 8637
module Conf;

public class Program
  public static method CountB(xs: IEnumerable<byte>): int
    var n := 0;
    for var x in xs do
      n++;
    end;
    return n;
  end CountB;

  public static method CountU(xs: IEnumerable<ushort>): int
    var n := 0;
    for var x in xs do
      n++;
    end;
    return n;
  end CountU;

  public static method CountI(xs: IEnumerable<uint>): int
    var n := 0;
    for var x in xs do
      n++;
    end;
    return n;
  end CountI;

  public static method CountL(xs: IEnumerable<long>): int
    var n := 0;
    for var x in xs do
      n++;
    end;
    return n;
  end CountL;

  public static method Main()
    var b0: byte := 250;
    var b1: byte := 255;
    Console.WriteLine(CountB(b0..b1));
    Console.WriteLine(CountB(b1..b1));
    var s1: ushort := 65535;
    Console.WriteLine(CountU(s1..s1));
    var i1: uint := 4294967295;
    Console.WriteLine(CountI(i1..i1));
    var lmax: long := long.MaxValue;
    Console.WriteLine(CountL(lmax..lmax));
  end Main;
end Program;
