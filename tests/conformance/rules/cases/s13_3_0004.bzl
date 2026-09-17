//! case: s13_3_0004
//! doc: spec
//! section: 13.3
//! title: a destructor with no visibility keyword is admitted
//! rule: A `destructor` carries no visibility keyword — for such a member
//!   no visibility choice exists to spell.
//! cites: 13.3, C.2, 28.4
//! expect: accept
module Conf;

public class Handle
  public constructor()
  end constructor;

  destructor
  end destructor;
end Handle;
