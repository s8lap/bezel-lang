//! case: s30_6_0001
//! doc: spec
//! section: 30.6
//! title: an async lambda may target a (string) -> Task<int> function type
//! rule: The function type of an async lambda has a `Task`, `Task<T>`, or
//!   `OwningTask<T>` return type. `Task<T>` for borrowing or value-typed
//!   results.
//! cites: 30.6
//! expect: accept
module Conf;

public class Program
  public method Setup()
    var quick: (string) -> Task<int> := async lambda(url) => await Task.FromResult(url.Length);
  end Setup;
end Program;
