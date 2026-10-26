//! case: l9_1_0013
//! doc: stdlib
//! section: 9.1
//! title: Parse(s, radix) and ToString(radix) are inverse
//! rule: stdlib §9.1: "The two are therefore inverse: `T.Parse(n.ToString(radix),
//!     radix)` is `n` for every value of every integer type and every admitted
//!     radix".
//! cites: 9.1
//! expect: run
//! stdout: 255
module Conf;

public class Program
  public static method Main()
    var n := 255;
    Console.WriteLine(int.Parse(n.ToString(16), 16));
  end Main;
end Program;
