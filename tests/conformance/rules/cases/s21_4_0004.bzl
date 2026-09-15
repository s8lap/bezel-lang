//! case: s21_4_0004
//! doc: spec
//! section: 21.4
//! title: the loop variable is freshly bound each iteration
//! rule: The loop variable is freshly bound on each iteration; assigning
//!   to the loop variable inside the body affects only that iteration — it
//!   does not alter how many iterations run, nor the next iteration's
//!   value.
//! cites: 21.4
//! expect: run
//! stdout: 99
module Conf;

public class Program
  public static method Main()
    for var i in 1..3 do
      i := 99;
      Console.WriteLine(i.ToString());
    end;
    Console.WriteLine("done");
  end Main;
end Program;
