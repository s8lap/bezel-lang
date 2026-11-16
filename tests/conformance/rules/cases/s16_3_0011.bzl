//! case: s16_3_0011
//! doc: spec
//! section: 16.3
//! title: a step that overshoots a ushort, sbyte or uint type range stops iteration through an IEnumerable
//! rule: Iteration stops silently, raising no `OverflowException`, before the first value that would pass the range's endpoint or fall outside `T`'s representable range, whichever comes first
//! cites: 16.3, 16.5
//! expect: run
//! output: [0,]
//!   [100,-50,]
//!   [0,3000000000,]
//! issue: 8637
module Conf;

public class Program
  public static method ShowU(xs: IEnumerable<ushort>): string
    var s := "";
    for var x in xs do
      s := s + $"{x},";
    end;
    return $"[{s}]";
  end ShowU;

  public static method ShowS(xs: IEnumerable<sbyte>): string
    var s := "";
    for var x in xs do
      s := s + $"{x},";
    end;
    return $"[{s}]";
  end ShowS;

  public static method ShowI(xs: IEnumerable<uint>): string
    var s := "";
    for var x in xs do
      s := s + $"{x},";
    end;
    return $"[{s}]";
  end ShowI;

  public static method Main()
    var us0: ushort := 0;
    var us9: ushort := 10;
    Console.WriteLine(ShowU(us0..us9 step 70000));
    var sb1: sbyte := 100;
    var sb2: sbyte := -100;
    Console.WriteLine(ShowS(sb1..sb2 step -150));
    var u0: uint := 0;
    var u9: uint := 4000000000;
    Console.WriteLine(ShowI(u0..u9 step 3000000000L));
  end Main;
end Program;
