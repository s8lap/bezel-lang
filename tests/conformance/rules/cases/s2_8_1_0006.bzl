//! case: s2_8_1_0006
//! doc: spec
//! section: 2.8.1
//! title: an aliased import of the file's own module is still a self-import
//! rule: An `import` naming the enclosing file's own module is rejected ("a
//!   module cannot import itself"); the self-loop is not a cycle, it is a
//!   mistake. An `as` alias renames the binding, it does not change which
//!   module is named.
//! cites: 2.8.1, 2.2, C.13
//! expect: reject
//! diagnostic: itself
module Conf;
import Conf as C;

public class Widget
  public constructor()
  end constructor;
end Widget;
