//! case: s29_5_0002
//! doc: spec
//! section: 29.5
//! title: yield break is the correct early-exit spelling inside an iterator method
//! rule: A method that uses `yield` cannot use plain `return`, including
//!   `return;`. Use `yield break` to exit early.
//! cites: 29.5
//! expect: accept
module Conf;

public class Program
  public static method Gen(): IEnumerable<int>
    yield return 1;
    yield break;
  end Gen;
end Program;
