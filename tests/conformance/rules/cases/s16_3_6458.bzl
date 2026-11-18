//! case: s16_3_6458
//! doc: spec
//! section: 16.3
//! title: a ushort step variable of 40000 over 0..65535 stops before the value
//!   outside ushort
//! rule: Iteration stops silently, raising no `OverflowException`, before the first
//!   value that would pass the range's endpoint or fall outside `T`'s
//!   representable range, whichever comes first
//! cites: 16.3
//! expect: run
//! stdout: [0,40000,]
module Conf;
public class Program
  public static method Show(xs: IEnumerable<ushort>): string
    var s := "";
    for var x in xs do s := s + $"{x},"; end;
    return $"[{s}]";
  end Show;
  public static method Main()
    var s0: ushort := 0;
    var s9: ushort := 65535;
    var ss: ushort := 40000;
    Console.WriteLine(Show(s0..s9 step ss));
  end Main;
end Program;
