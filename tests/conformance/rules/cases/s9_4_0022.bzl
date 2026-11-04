//! case: s9_4_0022
//! doc: spec
//! section: 9.4
//! title: a lambda inside a record constructor may read the captured receiver
//! rule: the store rejection above reads a lambda body inside the member body
//!   as the member body, and one inside a value kind's constructor body as a
//!   member body too, a function value outliving the construction window
//!   (§22.8) — a chain rooted at the captured `this` that reaches the value is
//!   rejected there.
//! cites: 22.7, 22.8
//! expect: run
//! stdout: 4
//! notes: twin of s9_4_0016 (one legal edit: the lambda reads `Hits` instead
//!   of storing to it), formed after every field is assigned.
module Conf;

public record Tally
  public property Hits: int;

  public constructor(hits: int)
    Hits := hits;
    var f: () -> int := lambda() => Hits;
    Console.WriteLine(f());
  end constructor;
end Tally;

public class Program
  public static method Main()
    _ := new Tally(4);
  end Main;
end Program;
