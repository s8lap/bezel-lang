//! case: s3_4_0001
//! doc: spec
//! section: 3.4
//! title: a user type may not be named after a reserved primitive alias
//! rule: The PascalCase standard-library names of the primitive value types
//!   and String are reserved type names: a program may not declare a user
//!   type whose name is one of them.
//! cites: 3.4
//! expect: reject
//! diagnostic: reserved
module Conf;

public class Int32
end Int32;
