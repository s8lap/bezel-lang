//! case: s2_7_0007
//! doc: spec
//! section: 2.7
//! title: an unimported module's presence in the build does not satisfy
//!   rule 2's prefix match
//! rule: Match the longest dotted prefix of the chain against an
//!   *imported* module's full name. A prefix matching no import is an error
//!   at `A`, even when a module of that name is known to the build —
//!   `Data.Store` is compiled as part of this build but never `import`ed by
//!   this file, so `Data` does not resolve.
//! cites: 2.7
//! expect: reject
//! diagnostic: Data
module Conf;

public class Program
  public static method Main()
    var x: Data.Store.Item;
  end Main;
end Program;
