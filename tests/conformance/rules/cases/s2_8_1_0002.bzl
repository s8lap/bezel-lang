//! case: s2_8_1_0002
//! doc: spec
//! section: 2.8.1
//! title: importing a distinct module is accepted
//! rule: An `import` naming the enclosing file's own module is rejected;
//!   importing a distinct module is unaffected.
//! cites: 2.8.1, C.13
//! expect: accept
module Conf;
import System.Threading;

public class Widget
end Widget;
