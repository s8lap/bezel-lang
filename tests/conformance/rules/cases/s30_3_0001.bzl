//! case: s30_3_0001
//! doc: spec
//! section: 30.3
//! title: Result is a legal blocking read outside any domain
//! rule: `Wait()` and `Result` are synchronous escape hatches for code
//!   running outside any domain — in practice, the synchronous portion of
//!   program startup: a non-`async` `Main`, and code it calls directly on
//!   the main thread.
//! cites: 30.3
//! expect: run
//! stdout: 42
module Conf;

public class Loader
  public static method LoadAsync(): Task<int>
    return Task.FromResult(42);
  end LoadAsync;
end Loader;

public class Program
  public static method Main()
    using var t := Loader.LoadAsync();
    Console.WriteLine(t.Result);
  end Main;
end Program;
