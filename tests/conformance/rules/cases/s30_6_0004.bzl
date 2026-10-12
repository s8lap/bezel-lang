//! case: s30_6_0004
//! doc: spec
//! section: 30.6
//! title: an expression-bodied async lambda targeting a (string) -> Task<int> function type
//! rule: `var quick: (string) -> Task<int> := async lambda(url) => await
//!   CountBytesAsync(url);` — the function type of an async lambda has a
//!   `Task`, `Task<T>`, or `OwningTask<T>` return type, and the wrapper comes
//!   from the closed target return slot when the body conforms to it.
//! cites: 30.6, 30.1
//! expect: run
//! stdout: 4
module Conf;

public class Count
  public static async method CountBytesAsync(url: string): Task<int>
    await Task.Delay(0);
    return url.Length;
  end CountBytesAsync;
end Count;

public class Program
  public static async method Main(): Task
    var quick: (string) -> Task<int> := async lambda(url) => await Count.CountBytesAsync(url);
    using var t := quick("abcd");
    var n := await t;
    Console.WriteLine(n);
  end Main;
end Program;
