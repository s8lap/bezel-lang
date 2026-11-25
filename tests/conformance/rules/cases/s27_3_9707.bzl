//! case: s27_3_9707
//! doc: spec
//! section: 27.3
//! title: a generic indexer slot substituted from the class type argument admits a matching lambda
//! rule: its inferred return type must equal the target's return slot or implicitly convert to it
//! cites: 27.3, 22.6, 14.5, 23.4
//! expect: run
//! output: 5
//! notes: Pins #9230 (pick 16a3dc690): an index position that supplies a function-typed target is judged
//!   like a call argument: the body is typed and its inferred return type must equal the slot's
//!   return type or implicitly convert to it, otherwise the lambda is a compile error at the position
//!   (the method twin was already refused; the indexer path admitted the lambda and the accessor
//!   called it through the wrong ABI). `C<string>` substitutes T := string at the slot before the
//!   check (at a call argument the same check applies to the slot after substitution), so `lambda()
//!   => "hello"` matches exactly and the program prints 5; a fix that judged the unsubstituted slot
//!   would fail here.
module Conf;

public class C<T>
  public property [f: () -> T]: int => 5;
  public constructor()
  end constructor;
end C;

public class Program
  public static method Main(): int
    using var c := new C<string>();
    var n := c[lambda() => "hello"];
    Console.WriteLine(n);
    return 0;
  end Main;
end Program;
