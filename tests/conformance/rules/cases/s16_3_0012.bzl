//! case: s16_3_0012
//! doc: spec
//! section: 16.3
//! title: a default-step range crossing the signed midpoint of an unsigned type drains every value
//! rule: `a..b` is inclusive on both ends
//! cites: 16.3, 16.5
//! expect: run
//! output: 16
//!   16
//!   16
//!   16
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

  public static method CountL(xs: IEnumerable<ulong>): int
    var n := 0;
    for var x in xs do
      n++;
    end;
    return n;
  end CountL;

  public static method Main()
    var b0: byte := 120;
    var b1: byte := 135;
    Console.WriteLine(CountB(b0..b1));
    var s0: ushort := 32760;
    var s1: ushort := 32775;
    Console.WriteLine(CountU(s0..s1));
    var i0: uint := 2147483640;
    var i1: uint := 2147483655;
    Console.WriteLine(CountI(i0..i1));
    var l0: ulong := 9223372036854775800;
    var l1: ulong := 9223372036854775815;
    Console.WriteLine(CountL(l0..l1));
  end Main;
end Program;
