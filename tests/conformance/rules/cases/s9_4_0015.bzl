//! case: s9_4_0015
//! doc: spec
//! section: 9.4
//! title: a lambda body inside a record method may not store into the captured receiver
//! rule: the store rejection above reads a lambda body inside the member body
//!   as the member body, and one inside a value kind's constructor body as a
//!   member body too, a function value outliving the construction window
//!   (§22.8) — a chain rooted at the captured `this` that reaches the value is
//!   rejected there.
//! cites: 27.5
//! expect: reject
//! diagnostic: receiver
module Conf;

public record Tally
  public property Hits: int;

  public constructor(hits: int)
    Hits := hits;
  end constructor;

  public method Bump()
    var f: () -> () := lambda() => do Hits := Hits + 1; end;
    f();
  end Bump;
end Tally;
