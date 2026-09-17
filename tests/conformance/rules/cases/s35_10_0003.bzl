//! case: s35_10_0003
//! doc: spec
//! section: 35.10
//! title: nameof on a choice case yields the case's bare identifier
//! rule: nameof(FetchOutcome.Failed) yields "Failed".
//! cites: 35.10, 17.9
//! expect: run
//! stdout: Failed
module Conf;

public choice Outcome
  Ok(value: int),
  Failed(message: string);
end Outcome;

public class Program
  public static method Main()
    Console.WriteLine(nameof(Outcome.Failed));
  end Main;
end Program;
