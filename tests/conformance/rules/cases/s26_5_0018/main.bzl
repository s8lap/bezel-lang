//! case: s26_5_0018
//! doc: spec
//! section: 26.5
//! title: instance members win over a same-named extension imported from another module
//! rule: Instance members win — by name. If the receiver's declared type
//!   declares any instance member of that name, instance resolution is
//!   committed and extension blocks are never consulted — regardless of
//!   which module declares the conflicting extension.
//! cites: 26.5
//! expect: run
//! stdout: instance
module App;
import Lib;
import Ext;

public class Program
  public static method Main()
    using var w := new Widget();
    Console.WriteLine(w.Describe());
  end Main;
end Program;
