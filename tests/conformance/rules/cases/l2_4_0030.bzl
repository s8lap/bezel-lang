//! case: l2_4_0030
//! doc: stdlib
//! section: 2.4
//! title: The null-forgiving operator turns Get's string? result into a string
//! rule: `x!` asserts non-nullness. If `x` is null at runtime,
//!   `NullReferenceException` is raised.
//! cites: 2.4, 7.8
//! expect: run
//! output: h
//!   npe
//! issue: 8096
//! notes: twin of l2_4_0021 (pairs the accepted with its rejecting form).
module Conf;

public class Program
  public static method Main()
    Environment.Set("BZ_H3_BANG", "h");
    var x: string := Environment.Get("BZ_H3_BANG")!;
    Console.WriteLine(x);
    try
      var y: string := Environment.Get("BZ_H3_BANG_ABSENT")!;
      Console.WriteLine(y);
    catch NullReferenceException => Console.WriteLine("npe");
    end try;
  end Main;
end Program;
