//! case: s2_3_0004
//! doc: spec
//! section: 2.3
//! title: a user module's export collides with an auto-imported bare name
//! rule: A user module exporting one of the §2.3 always-available bare
//!   names makes the bare name ambiguous in a file importing both, and no
//!   opt-out exists.
//! cites: 2.3, 2.2
//! expect: reject
//! diagnostic: ambiguous
module Conf;
import Lib;

public class Program
  public static method Main()
    using var b := new BitReader();
  end Main;
end Program;
