//! case: s35_3_0009
//! doc: spec
//! section: 35.3
//! title: a qualified payload-free case written with an empty argument list is a compile error
//! rule: "Construction is qualified, resolving exactly as enum constants do (§10.1): `FetchOutcome.NotModified` for a payload-free case (**a value expression, not a call**)" (§35.3, v0.340). `Outcome.Nothing` is a value of type `Outcome`; a choice type is not callable (§27.1), so `Outcome.Nothing()` has no meaning and is a compile error (owner ruling #7048, reading 1).
//! cites: 27.1, 10.1
//! expect: reject
//! diagnostic: no payload
//! issue: 7048
module Conf;

public choice Outcome
  Got(b: string),
  Nothing;
end Outcome;

public class Program
  public static method Main()
    var o := Outcome.Nothing();
    Console.WriteLine((case o of Got(var b) => b, Nothing => "nothing"));
  end Main;
end Program;
