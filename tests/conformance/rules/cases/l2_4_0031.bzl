//! case: l2_4_0031
//! doc: stdlib
//! section: 2.4
//! title: A null test narrows Get's string? result bound to a local
//! rule: a null test in either spelling — `x <> null` or `x is not null` (narrowing
//!   the true branch)
//! cites: 2.4, 7.4
//! expect: run
//! output: 3
//!   none
//! issue: 8096
//! notes: twin of l2_4_0023 (pairs the accepted with its rejecting form).
module Conf;

public class Program
  public static method Main()
    Environment.Set("BZ_H3_NARROW", "abc");
    var s := Environment.Get("BZ_H3_NARROW");
    if s <> null then
      Console.WriteLine(s.Length);
    else
      Console.WriteLine("none");
    end;
    var t := Environment.Get("BZ_H3_NARROW_ABSENT");
    if t <> null then
      Console.WriteLine(t.Length);
    else
      Console.WriteLine("none");
    end;
  end Main;
end Program;
