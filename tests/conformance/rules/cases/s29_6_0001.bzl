//! case: s29_6_0001
//! doc: spec
//! section: 29.6
//! title: yield return inside a try block with a catch is rejected
//! rule: `yield return` or `yield break` inside a `try` block whose
//!   surrounding statement has a `catch`: forbidden.
//! cites: 29.6
//! expect: reject
//! diagnostic: yield
module Conf;

public class Program
  public static method Gen(): IEnumerable<int>
    try
      yield return 1;
    catch
      InvalidOperationException => Console.WriteLine("oops"),
    end try;
  end Gen;
end Program;
