//! case: s29_6_0003
//! doc: spec
//! section: 29.6
//! title: yield is forbidden inside a catch arm's body
//! rule: `yield` inside a catch arm's body or a `finally` block: forbidden.
//! cites: 29.6
//! expect: reject
//! diagnostic: yield
module Conf;

public class Program
  public static method Gen(): IEnumerable<int>
    try
      throw new InvalidOperationException();
    catch
      InvalidOperationException => do
        yield return 1;
      end,
    end try;
  end Gen;
end Program;
