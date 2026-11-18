//! case: s16_3_6454
//! doc: spec
//! section: 16.3
//! title: a uint step read from a field steps a uint range
//! rule: `Step` is independent of `T`'s range, so a step beyond that range is
//!   **accepted at construction**
//! cites: 16.3
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
    var h := new Holder();
    Console.WriteLine(Show(u0..u9 step h.Stp));
  end Main;
end Program;
public class Holder
  public Stp: uint := 3000000000U;
end Holder;
