//! case: s16_3_6456
//! doc: spec
//! section: 16.3
//! title: a byte step variable of 128 over 0..255 yields 0 and 128
//! rule: over `byte`, `0..10 step 256` yields only `0`, `0..255 step 128` yields
//!   `0` and `128`, and `200..255 step 100` yields `200`
//! cites: 16.3
//! expect: run
//! stdout: [0,128,]
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
    var bz: byte := 128;
    Console.WriteLine(Show(b0..b9 step bz));
  end Main;
end Program;
