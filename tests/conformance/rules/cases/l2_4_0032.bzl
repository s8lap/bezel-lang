//! case: l2_4_0032
//! doc: stdlib
//! section: 2.4
//! title: The null-coalescing operator gives Get's string? result a string fallback
//! rule: `a ?? b` returns `a` if non-null, else `b`.
//! cites: 2.4, 7.7
//! expect: run
//! output: v
//!   d
//! issue: 8096
//! notes: twin of l2_4_0021 (pairs the accepted with its rejecting form).
module Conf;

public class Program
  public static method Main()
    Environment.Set("BZ_H3_COAL", "v");
    var a: string := Environment.Get("BZ_H3_COAL") ?? "d";
    var b: string := Environment.Get("BZ_H3_COAL_ABSENT") ?? "d";
    Console.WriteLine(a);
    Console.WriteLine(b);
  end Main;
end Program;
