//! case: s29_6_0004
//! doc: spec
//! section: 29.6
//! title: yield is forbidden inside a finally block
//! rule: `yield` inside a catch arm's body or a `finally` block: forbidden.
//! cites: 29.6
//! expect: reject
//! diagnostic: yield
module Conf;

public class Program
  public static method Gen(): IEnumerable<int>
    try
      Console.WriteLine("work");
    finally
      yield return 1;
    end try;
  end Gen;
end Program;
