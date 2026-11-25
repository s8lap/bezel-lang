//! case: l7_4_9950
//! doc: stdlib
//! section: 7.4
//! title: Console.In.Read() returns -1 at end of input, so a read-until-minus-one loop counts no
//!   characters
//! rule: next char as int, or -1 at EOF
//! cites: 7.4, 2.1
//! expect: run
//! output: count=0 last=-1
//! notes: Pins #9260. The corpus runner supplies an empty stdin, so the first Read() is already at end
//!   of input and returns -1; the loop body never runs: k = 0, n = -1.
module Conf;
public class Program
  public static method Main(): int
    var n := Console.In.Read();
    var k := 0;
    while n <> -1 do
      k := k + 1;
      n := Console.In.Read();
    end;
    Console.WriteLine($"count={k} last={n}");
    return 0;
  end Main;
end Program;
