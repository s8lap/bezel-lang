//! case: s21_10_0007
//! doc: spec
//! section: 21.10
//! title: a choice may not be declared as a local type
//! rule: "`choice` may **not** be declared locally (its payload-derived
//!   three-way classification, owning-match consume obligations, and coverage
//!   machinery are a heavier admission deserving its own study)."
//! cites: 21.10, C.1
//! expect: reject
//! diagnostic: choice
module Conf;

public class Program
  public static method Main()
    choice Outcome
      Ok(value: int),
      Failed(reason: string);
    end Outcome;
    Console.WriteLine("done");
  end Main;
end Program;
