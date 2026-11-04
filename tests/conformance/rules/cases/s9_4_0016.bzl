//! case: s9_4_0016
//! doc: spec
//! section: 9.4
//! title: a lambda body inside a record constructor may not store into the captured receiver
//! rule: the store rejection above reads a lambda body inside the member body
//!   as the member body, and one inside a value kind's constructor body as a
//!   member body too, a function value outliving the construction window
//!   (§22.8) — a chain rooted at the captured `this` that reaches the value is
//!   rejected there.
//! cites: 22.7, 22.8
//! expect: reject
//! diagnostic: receiver
//! notes: the constructor's own `Hits := hits;` is legal (s9_4_0004); only the
//!   store inside the lambda is the defect, formed after every field is
//!   assigned so the capture itself is an admitted read of `this`.
module Conf;

public record Tally
  public property Hits: int;

  public constructor(hits: int)
    Hits := hits;
    var f: () -> () := lambda() => do Hits := 0; end;
    f();
  end constructor;
end Tally;
