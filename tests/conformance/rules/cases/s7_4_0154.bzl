//! case: s7_4_0154
//! doc: spec
//! section: 7.4
//! title: a const copy formed inside the guard carries the narrowing into a lambda calling Math.Min
//! rule: the remedy is a `const` copy, tested or formed from the narrowed value (`const t := s;` inside the guard)
//! cites: 2.2, 27.5
//! expect: run
//! output: 3
//!   0
//! issue: 7660
//! notes: Accepting twin of the lambda reject row.
module Conf;

public class Program
  public static method Run(d: int?): int
    if d <> null then
      const t := d;
      var g := lambda() => Math.Min(t, 5);
      return g();
    end if;
    return 0;
  end Run;

  public static method Main()
    Console.WriteLine(Run(3));
    Console.WriteLine(Run(null));
  end Main;
end Program;
