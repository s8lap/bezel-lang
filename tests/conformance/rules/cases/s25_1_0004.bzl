//! case: s25_1_0004
//! doc: spec
//! section: 25.1
//! title: an operator overload declared without public is rejected
//! rule: `public` is required: an operator invoked through `a + b` syntax has no
//!   call-site spelling that could name a restricted member, so any other
//!   visibility is a compile error.
//! cites: 25
//! expect: reject
//! diagnostic: public
module Conf;

public record Weight(Kg: double)
  internal static method operator +(a: Weight, b: Weight): Weight =>
    new Weight(a.Kg + b.Kg);
end Weight;
