//! case: s21_7_0002
//! doc: spec
//! section: 21.7
//! title: a method with no return type uses return to exit early
//! rule: A method or accessor with no return type uses `return;` to exit
//!   early; falling off the end is implicit.
//! cites: 21.7
//! expect: run
//! stdout: negative
module Conf;

public class Program
  public static method Announce(n: int)
    if n < 0 then
      Console.WriteLine("negative");
      return;
    end if;
    Console.WriteLine("nonnegative");
  end Announce;

  public static method Main()
    Announce(-5);
  end Main;
end Program;
