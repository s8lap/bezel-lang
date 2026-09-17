//! case: s11_7_0017
//! doc: spec
//! section: 11.7
//! title: a closed class may not be subclassed from another module
//! rule: Classes derived from a `closed` base must be declared in the same
//!   module; the compiler rejects subclasses in other modules.
//! cites: 11.7, C.1
//! expect: reject
//! diagnostic: closed
module App;
import Lib;

public sealed class Circle: Shape
  public constructor()
  end constructor;

  public override method Area(): double => 1.0;
end Circle;
