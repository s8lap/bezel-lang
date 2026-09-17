//! case: s21_5_0005
//! doc: spec
//! section: 21.5
//! title: a break targeting a loop wholly inside the finally is admitted
//! rule: Loops wholly inside the `finally`, and jumps among them, are
//!   unaffected by the finally/loop-jump restriction.
//! cites: 21.5, 31.2
//! expect: run
//! stdout: try
module Conf;

public class Program
  public static method Main()
    try
      Console.WriteLine("try");
    finally
      inner: while true do
        Console.WriteLine("inner");
        break inner;
      end inner;
    end try;
    Console.WriteLine("after");
  end Main;
end Program;
