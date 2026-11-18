//! case: s16_3_6453
//! doc: spec
//! section: 16.3
//! title: a uint step variable above int range steps an exclusive uint range
//! rule: Iteration stops silently, raising no `OverflowException`, before the first
//!   value that would pass the range's endpoint or fall outside `T`'s
//!   representable range, whichever comes first
//! cites: 16.3, 16.4
//! expect: run
//! stdout: [0,3000000000,]
module Conf;
public class Program
  public static method Show(xs: IEnumerable<uint>): string
    var s := "";
    for var x in xs do s := s + $"{x},"; end;
    return $"[{s}]";
  end Show;
  public static method Main()
    var u0: uint := 0;
    var u9: uint := 4000000000;
    var st: uint := 3000000000;
    Console.WriteLine(Show(u0..<u9 step st));
  end Main;
end Program;
