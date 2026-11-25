//! case: s27_3_9701
//! doc: spec
//! section: 27.3
//! title: an int-returning lambda at a () -> string indexer slot is rejected on a read
//! rule: its inferred return type must equal the target's return slot or implicitly convert to it
//! cites: 27.3, 22.6, 23.4, 5.1
//! expect: reject
//! diagnostic: not implicitly convertible
//! notes: Pins #9230 (pick 16a3dc690): an index position that supplies a function-typed target is judged
//!   like a call argument: the body is typed and its inferred return type must equal the slot's
//!   return type or implicitly convert to it, otherwise the lambda is a compile error at the position
//!   (the method twin was already refused; the indexer path admitted the lambda and the accessor
//!   called it through the wrong ABI). Deletion test: with `lambda() => "hello"` the program compiles
//!   (accepting twin s27_3_9705).
module Conf;

public class C
  public property [f: () -> string]: int => f().Length;
  public constructor()
  end constructor;
end C;

public class Program
  public static method Main(): int
    using var c := new C();
    var n := c[lambda() => 7];
    Console.WriteLine(n);
    return 0;
  end Main;
end Program;
