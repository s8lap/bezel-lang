//! case: s2_8_1_0001
//! doc: spec
//! section: 2.8.1
//! title: a module cannot import itself
//! rule: An `import` naming the enclosing file's own module is rejected
//!   ("a module cannot import itself").
//! cites: 2.8.1, C.13
//! expect: reject
//! diagnostic: itself
module Conf;
import Conf;

public class Widget
end Widget;
