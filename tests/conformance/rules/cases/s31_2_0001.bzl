//! case: s31_2_0001
//! doc: spec
//! section: 31.2
//! title: a try statement with neither catch nor finally is rejected
//! rule: A `try` statement consists of a protected block, optionally followed
//!   by a `catch` block, optionally followed by a `finally` block. At least
//!   one of `catch` or `finally` must be present.
//! cites: 31.2
//! expect: reject
//! diagnostic: must be present
module Conf;

public class Program
  public static method Main()
    try
      Console.WriteLine("working");
    end try;
  end Main;
end Program;
