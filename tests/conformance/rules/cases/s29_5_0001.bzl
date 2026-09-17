//! case: s29_5_0001
//! doc: spec
//! section: 29.5
//! title: plain return is not allowed inside an iterator method
//! rule: A method that uses `yield` cannot use plain `return`, including
//!   `return;`. Use `yield break` to exit early.
//! cites: 29.5
//! expect: reject
//! diagnostic: plain return
module Conf;

public class Program
  public static method Gen(): IEnumerable<int>
    yield return 1;
    return;
  end Gen;
end Program;
