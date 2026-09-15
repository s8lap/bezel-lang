//! case: s15_4_0012
//! doc: spec
//! section: 15.4
//! title: the empty literal is always rejected for an inline-array target
//! rule: The empty literal `[]` is admitted for every collection target but is
//!   always rejected for inline arrays — a literal must supply exactly N >= 1
//!   elements (§15.4, §15.1).
//! cites: 15.4, 15.1
//! expect: reject
//! diagnostic: empty
module Conf;

public class Program
  public static method Run()
    var a: [3]int := [];
    Console.WriteLine(a.Length);
  end Run;
end Program;
