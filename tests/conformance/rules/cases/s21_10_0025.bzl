//! case: s21_10_0025
//! doc: spec
//! section: 21.10
//! title: two sibling do blocks may each declare a local record with the same name
//! rule: "A local type's name is in scope from its declaration statement to the end of the"
//! cites: 21.6
//! expect: run
//! output: 6
//!   hi!
module Conf;

public class Program
  public static method Main()
    do
      record R1(a: int)
        public method G(): int => a * 2;
      end R1;
      Console.WriteLine(new R1(3).G());
    end;
    do
      record R1(s: string)
        public method G(): string => s + "!";
      end R1;
      Console.WriteLine(new R1("hi").G());
    end;
  end Main;
end Program;
