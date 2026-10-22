//! case: s35_3_0010
//! doc: spec
//! section: 35.3
//! title: a qualified payload-free case, a qualified payload case and a bare declared-result case are accepted
//! rule: "Construction is qualified, resolving exactly as enum constants do (§10.1): `FetchOutcome.NotModified` for a payload-free case (**a value expression, not a call**)" (§35.3, v0.340). `Outcome.Nothing` is a value of type `Outcome`; a choice type is not callable (§27.1), so `Outcome.Nothing()` has no meaning and is a compile error (owner ruling #7048, reading 1). Control for #7048: `Outcome.Nothing`, `Outcome.Got("x")` and a bare `Nothing` in a declared-result position (§35.3) are the accepted spellings.
//! cites: 35.5
//! expect: run
//! output: nothing
//!   x
//!   nothing
module Conf;

public choice Outcome
  Got(b: string),
  Nothing;
end Outcome;

public class Program
  public static method Bare(): Outcome
    return Nothing;
  end Bare;

  public static method Show(o: Outcome): string
    return (case o of Got(var b) => b, Nothing => "nothing");
  end Show;

  public static method Main()
    Console.WriteLine(Show(Outcome.Nothing));
    Console.WriteLine(Show(Outcome.Got("x")));
    Console.WriteLine(Show(Bare()));
  end Main;
end Program;
