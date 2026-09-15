//! case: s35_1_0006
//! doc: spec
//! section: 35.1
//! title: owned on a primitive-typed payload field is rejected
//! rule: The owned payload marker's admissibility follows §28.1's type requirement:
//!   admissible on class types, interface types, and bare type parameters; a compile
//!   error on any other type.
//! expect: reject
//! diagnostic: owned
module Conf;

public choice Outcome
  Ready(owned code: int);   // ERROR: `owned` on a primitive payload is a compile error
end Outcome;
