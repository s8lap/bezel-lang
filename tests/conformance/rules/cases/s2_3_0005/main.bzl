//! case: s2_3_0005
//! doc: spec
//! section: 2.3
//! title: qualifying the colliding name resolves the ambiguity
//! rule: A user module exporting an always-available bare name makes the
//!   bare name ambiguous; qualification (the module name or an alias, §2.2)
//!   is the remedy — no opt-out exists, but qualifying reaches the intended
//!   declaration.
//! cites: 2.3, 2.2
//! expect: run
//! stdout: lib
module Conf;
import Lib;

public class Program
  public static method Main()
    using var b := new Lib.BitReader();
    Console.WriteLine(b.Tag());
  end Main;
end Program;
