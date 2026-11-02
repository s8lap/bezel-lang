//! case: s8_1_0004
//! doc: spec
//! section: 8.1
//! title: GetType is callable through a null-conditional on a nullable task
//! rule: `v.GetType()` returns the `Type` value for the runtime type of `v`.
//! cites: 8.2, 7.5, 8.1, 30.3
//! expect: run
//! issue: 6859
//! output: True
module Conf;

public class Program
  public static method Main(): int
    using var t0 := Task.FromResult(2);
    var t: Task<int>? := t0;
    var b := t?.GetType()?.IsClass ?? false;
    Console.WriteLine(b.ToString());
    return 0;
  end Main;
end Program;
