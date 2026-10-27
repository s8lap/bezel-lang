//! case: s28_12_0026
//! doc: spec
//! section: 28.12
//! title: await using declared Object over a runtime-managed value disposes nothing
//! rule: Over a borrow right-hand side (a call without an `owned` return, a field,
//!   an indexer, a borrow-state binding) the binding assumes no obligation, so
//!   scope exit disposes nothing and awaits nothing: the form is the same
//!   compile-time pin as under `using var`, with the same freeze on the source.
//! cites: 28.12, 28.9, 28.10
//! expect: run
//! issue: 7337
//! output: [1]

module Conf;

public class Program
  public static async method Main(): Task
    var o := 1;
    await using var x: Object := o.ToString();
    Console.WriteLine($"[{x}]");
  end Main;
end Program;
