//! case: s26_5_0019
//! doc: spec
//! section: 26.5
//! title: a failed instance lookup does not fall back to an extension from another module
//! rule: If the committed instance lookup fails — no applicable overload —
//!   the call is a compile error citing the instance members; it does not
//!   fall back to an extension, even one visible from an imported module.
//! cites: 26.5
//! expect: reject
//! diagnostic: Describe
module App;
import Lib;
import Ext;

public class Program
  public static method Main()
    using var w := new Widget();
    Console.WriteLine(w.Describe());
  end Main;
end Program;
