//! case: s16_3_6455
//! doc: spec
//! section: 16.3
//! title: a uint step passed as a method parameter steps a uint range
//! rule: `Step` is independent of `T`'s range, so a step beyond that range is
//!   **accepted at construction**
//! cites: 16.3
//! expect: run
//! stdout: [0,3000000000,]
module Conf;
public class Program
  public static method Par(lo: uint, hi: uint, st: uint): string => Show(lo..hi step st);
  public static method Show(xs: IEnumerable<uint>): string
    var s := "";
    for var x in xs do s := s + $"{x},"; end;
    return $"[{s}]";
  end Show;
  public static method Main()
    Console.WriteLine(Par(0, 4000000000, 3000000000));
  end Main;
end Program;
