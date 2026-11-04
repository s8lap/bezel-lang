//! case: s7_4_0156
//! doc: spec
//! section: 7.4
//! title: a string narrowed by an early return is admitted at Math.Max
//! rule: an early exit: after `if x = null then return; end;` or `if x is null then return; end;`
//! cites: 2.2, 14.2
//! expect: run
//! output: z
//!   none
//! issue: 7660
module Conf;

public class Program
  public static method Pick(s: string?): string
    if s = null then return "none"; end if;
    return Math.Max(s, "b");
  end Pick;

  public static method Main()
    Console.WriteLine(Pick("z"));
    Console.WriteLine(Pick(null));
  end Main;
end Program;
