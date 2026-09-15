//! case: s35_3_0001
//! doc: spec
//! section: 35.3
//! title: qualified case construction resolves a payload-free and a payload case
//! rule: Construction is qualified, resolving exactly as enum constants do:
//!   FetchOutcome.NotModified for a payload-free case, FetchOutcome.Failed(504, ...)
//!   for a payload case.
//! expect: run
//! stdout: 404: not found
module Conf;

public choice Outcome
  Failed(status: int, message: string),
  Ok;
end Outcome;

public class Program
  public static method Main()
    var a := Outcome.Ok;
    var b := Outcome.Failed(404, "not found");
    case a of
      Ok           => Console.WriteLine("ok"),
      Failed(_, _) => Console.WriteLine("failed")
    end case;
    case b of
      Ok                   => Console.WriteLine("ok"),
      Failed(var s, var m) => Console.WriteLine($"{s}: {m}")
    end case;
  end Main;
end Program;
