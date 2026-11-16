//! case: s16_3_0010
//! doc: spec
//! section: 16.3
//! title: the §16.3 byte step examples drain to the same values through an IEnumerable<byte>
//! rule: Iteration stops silently, raising no `OverflowException`, before the first value that would pass the range's endpoint or fall outside `T`'s representable range, whichever comes first
//! cites: 16.3, 16.5
//! expect: run
//! output: [0,]
//!   [0,128,]
//!   [200,]
//! issue: 8637
module Conf;

public class Program
  public static method Show(xs: IEnumerable<byte>): string
    var s := "";
    for var x in xs do
      s := s + $"{x},";
    end;
    return $"[{s}]";
  end Show;

  public static method Main()
    var zero: byte := 0;
    var ten: byte := 10;
    var top: byte := 255;
    var t200: byte := 200;
    Console.WriteLine(Show(zero..ten step 256));
    Console.WriteLine(Show(zero..top step 128));
    Console.WriteLine(Show(t200..top step 100));
  end Main;
end Program;
