//! case: s27_3_0912
//! doc: spec
//! section: 27.3
//! title: an int lambda body at an (int) -> long? target converts through long to long?
//! rule: its inferred return type must equal the target's return slot or implicitly convert to it
//!   ... `S` converts to `T?` via `S → T → T?`, so an `int` is admissible where a `long?` is
//!   expected
//! cites: 5.1
//! expect: run
//! output: 4
//! issue: 8168
module Conf;

public class Program
  public static method Main()
    var f: (int) -> long? := lambda(x) => x;
    Console.WriteLine(f(4) ?? 0L);
  end Main;
end Program;
