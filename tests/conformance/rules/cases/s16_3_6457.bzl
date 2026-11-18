//! case: s16_3_6457
//! doc: spec
//! section: 16.3
//! title: a byte step variable of 200 over 0..255 stops before the value outside
//!   byte
//! rule: Iteration stops silently, raising no `OverflowException`, before the first
//!   value that would pass the range's endpoint or fall outside `T`'s
//!   representable range, whichever comes first
//! cites: 16.3
//! expect: run
//! stdout: [0,200,]
module Conf;
public class Program
  public static method Show(xs: IEnumerable<byte>): string
    var s := "";
    for var x in xs do s := s + $"{x},"; end;
    return $"[{s}]";
  end Show;
  public static method Main()
    var b0: byte := 0;
    var b9: byte := 255;
    var bs: byte := 200;
    Console.WriteLine(Show(b0..b9 step bs));
  end Main;
end Program;
