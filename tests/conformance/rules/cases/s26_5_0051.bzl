//! case: s26_5_0051
//! doc: spec
//! section: 26.5
//! title: a declared instance member Result of Task<T> blocks an extension property of the same name
//! rule: instance resolution is committed and extension blocks are **never consulted** for that name
//!   on that receiver
//! cites: 30.3, 26.3
//! expect: run
//! output: 7
module Conf;

extension Task<T>
  public property Result: int => 5;
end extension;

public class Program
  public static method Main(): int
    using var b := Task.FromResult(7);
    Console.WriteLine(b.Result.ToString());
    return 0;
  end Main;
end Program;
