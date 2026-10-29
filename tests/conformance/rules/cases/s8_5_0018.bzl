//! case: s8_5_0018
//! doc: spec
//! section: 8.5
//! title: an enum declaring ToString replaces the default for direct, boxed and interpolated use
//! rule: An enum may replace its default by declaring a `ToString(): string` member
//! cites: 8.5, 10.5
//! expect: run
//! output: mine2
//!   mine2
//!   mine2
module Conf;

enum Mine2
  A, B;
  public override method ToString(): string => "mine2";
end Mine2;

public class Program
  public static method Main()
    Console.WriteLine(Mine2.A.ToString());
    var o2: Object := Mine2.B;
    Console.WriteLine(o2);
    Console.WriteLine($"{Mine2.A}");
  end Main;
end Program;
