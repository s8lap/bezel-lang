//! case: l4_20_0004
//! doc: stdlib
//! section: 4.20
//! title: a key decremented to zero or below is not removed and still counts toward Count
//! rule: A key decremented to zero or below is not removed -- it keeps its
//!   position, counts toward Count, and CountOf returns its possibly
//!   negative value; only Remove deletes it.
//! cites: 4.20
//! expect: run
//! stdout: 1
module Conf;

public class Program
  public static method Main()
    using var tally := new Counter<string>();
    tally.Increment("a");
    tally.Increment("a", -1);
    Console.WriteLine(tally.Count);
  end Main;
end Program;
