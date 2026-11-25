//! case: s27_3_9705
//! doc: spec
//! section: 27.3
//! title: a lambda whose return type matches the function-typed indexer slot is admitted
//! rule: its inferred return type must equal the target's return slot or implicitly convert to it
//! cites: 27.3, 22.6, 23.4
//! expect: run
//! output: 5
//! notes: Pins #9230 (pick 16a3dc690): an index position that supplies a function-typed target is judged
//!   like a call argument: the body is typed and its inferred return type must equal the slot's
//!   return type or implicitly convert to it, otherwise the lambda is a compile error at the position
//!   (the method twin was already refused; the indexer path admitted the lambda and the accessor
//!   called it through the wrong ABI). The accepting twin of s27_3_9701: the body `"hello"` is a
//!   string, equal to the slot's return type, and the accessor returns its Length, 5.
module Conf;

public class C
  public property [f: () -> string]: int => f().Length;
  public constructor()
  end constructor;
end C;

public class Program
  public static method Main(): int
    using var c := new C();
    var n := c[lambda() => "hello"];
    Console.WriteLine(n);
    return 0;
  end Main;
end Program;
