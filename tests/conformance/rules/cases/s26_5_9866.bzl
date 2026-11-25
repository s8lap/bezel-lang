//! case: s26_5_9866
//! doc: spec
//! section: 26.5
//! title: A nested record's bare name in its enclosing class body does not supply ReferenceEquals
//! rule: a call or read `T.M` that none of them supplies names no member and is the same compile error
//! cites: 26.5, 8.1, 11.6
//! expect: reject
//! diagnostic: no member
//! issue: 9021
//! notes: Harvested from the #9021 review (r16_qualified_nested_record). Inside `Outer`, the bare
//!   name `R` resolves to the nested record (2.7, 11.6) and is a type name chain head; through
//!   it nothing supplies `ReferenceEquals` (26.5 rule 2), exactly as for a module-level record.
module Conf;

public class Outer
  public record R(X: int);

  public static method T(): bool
    var x := new R(1);
    return R.ReferenceEquals(x, x);
  end T;
end Outer;
