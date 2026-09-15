//! case: s29_5_0003
//! doc: spec
//! section: 29.5
//! title: plain return is barred in an async iterator too
//! rule: This rule applies equally to synchronous iterators (returning
//!   `IEnumerable<T>`) and async iterators (declared `async`, returning
//!   `IAsyncEnumerable<T>`). ... `yield` controls the flow, and `yield
//!   break` terminates it.
//! cites: 29.5
//! expect: reject
//! diagnostic: plain return
module Conf;

public class Program
  public static async method Gen(): IAsyncEnumerable<int>
    yield return 1;
    return;
  end Gen;
end Program;
