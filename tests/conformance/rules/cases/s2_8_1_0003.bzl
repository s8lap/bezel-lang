//! case: s2_8_1_0003
//! doc: spec
//! section: 2.8.1
//! title: declaring an implementation-provided module name is a build error
//! rule: The standard-library modules are implicit members of every build's
//!   artifact set; they participate in duplicate-name detection — a root
//!   compilation declaring a module name the implementation provides
//!   (`module System;`, `module System.Threading;`, …) is the rule-3 build
//!   error.
//! cites: 2.8.1, C.13
//! expect: reject
//! diagnostic: System
module System;

public class Widget
  public constructor()
  end constructor;
end Widget;
