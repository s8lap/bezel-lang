//! case: s3_4_0002
//! doc: spec
//! section: 3.4
//! title: a non-reserved type name compiles
//! rule: The PascalCase standard-library names of the primitive value types
//!   and String are reserved type names; any other name is unaffected.
//! cites: 3.4
//! expect: accept
module Conf;

public class Kilobytes
end Kilobytes;
