//! case: s3_4_0003
//! doc: spec
//! section: 3.4
//! title: an ordinary library type name such as Stream may be shadowed by a user type
//! rule: This does not reserve other standard-library type names — only the
//!   PascalCase aliases of the primitive value types and String; an ordinary
//!   library type such as Array or Stream may still be shadowed by a user type.
//! cites: 3.4
//! expect: accept
module Conf;

public class Stream
end Stream;
