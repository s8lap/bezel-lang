//! case: l2_4_0029
//! doc: stdlib
//! section: 2.4
//! title: A null-conditional access on Get's string? result yields the length or null
//! rule: public static method Get(name: string): string?;
//! cites: 2.4, 7.5, 7.7
//! expect: run
//! output: 5
//!   -1
//! issue: 8096
//! notes: twin of l2_4_0023 (pairs the accepted with its rejecting form).
module Conf;

public class Program
  public static method Main()
    Environment.Set("BZ_H3_LEN", "eeeee");
    var n := Environment.Get("BZ_H3_LEN")?.Length;
    Console.WriteLine(n ?? -1);
    var m := Environment.Get("BZ_H3_LEN_ABSENT")?.Length;
    Console.WriteLine(m ?? -1);
  end Main;
end Program;
