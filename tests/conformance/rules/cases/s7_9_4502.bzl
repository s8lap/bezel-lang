//! case: s7_9_4502
//! doc: spec
//! section: 7.9
//! title: a non-defaultable local assigned on every path may be read in an
//!   interpolation hole
//! rule: a local of **non-defaultable** type ([§17.11](#sec-the-default-operator))
//!   must be assigned before it is read
//! cites: 21.1, 18.5
//! expect: run
//! output: s=y
//! issue: 8392
module Conf;

public class Program
  public static method Main()
    var s: string;
    if 1 > 2 then s := "x"; else s := "y"; end;
    Console.WriteLine($"s={s}");
  end Main;
end Program;
