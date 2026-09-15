//! case: s35_5_0007
//! doc: spec
//! section: 35.5
//! title: a payload case matched bare with no arity is rejected
//! rule: A payload case may not be matched bare: Fetched without parentheses is a
//!   compile error naming the arity — matching a payload while ignoring it is
//!   spelled visibly as Fetched(_).
//! expect: reject
//! diagnostic: payload pattern
module Conf;

public choice Outcome
  Fetched(status: int),
  NotModified;
end Outcome;

public class Program
  public static method Main()
    var outcome := Outcome.Fetched(200);
    case outcome of
      Fetched      => Console.WriteLine("ok"),   // ERROR: payload case matched with no payload pattern
      NotModified  => Console.WriteLine("nm")
    end case;
  end Main;
end Program;
