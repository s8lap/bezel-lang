//! case: s7_4_0903
//! doc: spec
//! section: 7.4
//! title: a ReadOnlyStringSpan? parameter narrowed by an early return admits member access on the
//!   surviving
//! rule: an early exit: after `if x = null then return; end;` or `if x is null then return; end;`
//! cites: 7.4
//! expect: run
//! output: cde
//!   5
//!   -1
module Conf;

public class Program
  static method F(p: ReadOnlyStringSpan?): int
    if p = null then
      return -1;
    end if;
    Console.WriteLine(p.ToString());
    return p.Length + p.Start;
  end F;

  public static method Main()
    var w := "abcdef";
    Console.WriteLine(F(new ReadOnlyStringSpan(w, 2, 3)));
    Console.WriteLine(F(null));
  end Main;
end Program;
