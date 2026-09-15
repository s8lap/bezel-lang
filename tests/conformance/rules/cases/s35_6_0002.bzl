//! case: s35_6_0002
//! doc: spec
//! section: 35.6
//! title: a refutable slot arm contributes nothing to exhaustiveness
//! rule: Guarded arms and refutable-slot arms contribute nothing to the static proof
//!   for choice scrutinees.
//! expect: warn
//! diagnostic: exhaustive
module Conf;

public choice Outcome
  Ok(value: int),
  Error(message: string);
end Outcome;

public class Program
  public static method Describe(o: Outcome): string =>
      (case o of
        Ok(0)    => "zero",     // refutable slot: contributes nothing to exhaustiveness
        Error(_) => "error");   // WARN: `Ok` is not fully covered, no `_` arm supplied

  public static method Main()
    Console.WriteLine(1);
  end Main;
end Program;
