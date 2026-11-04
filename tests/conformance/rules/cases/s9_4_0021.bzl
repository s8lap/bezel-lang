//! case: s9_4_0021
//! doc: spec
//! section: 9.4
//! title: a lambda inside a record method may read the captured receiver
//! rule: the store rejection above reads a lambda body inside the member body
//!   as the member body, and one inside a value kind's constructor body as a
//!   member body too, a function value outliving the construction window
//!   (§22.8) — a chain rooted at the captured `this` that reaches the value is
//!   rejected there.
//! cites: 27.5
//! expect: run
//! stdout: 6
//! notes: twin of s9_4_0015 (one legal edit: the lambda reads `Hits` instead
//!   of storing to it).
module Conf;

public record Tally
  public property Hits: int;

  public constructor(hits: int)
    Hits := hits;
  end constructor;

  public method Next(): int
    var f: () -> int := lambda() => Hits + 1;
    return f();
  end Next;
end Tally;

public class Program
  public static method Main()
    var t := new Tally(5);
    Console.WriteLine(t.Next());
  end Main;
end Program;
