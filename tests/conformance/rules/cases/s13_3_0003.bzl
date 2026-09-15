//! case: s13_3_0003
//! doc: spec
//! section: 13.3
//! title: a destructor given an explicit visibility keyword is rejected
//! rule: A `destructor` is a member kind user code can never invoke or name
//!   at a use site — for such a member no visibility choice exists to
//!   spell, and it carries no visibility keyword.
//! cites: 13.3, C.2, 28.4
//! expect: reject
//! diagnostic: no visibility keyword
module Conf;

public class Handle
  public constructor()
  end constructor;

  public destructor
  end destructor;
end Handle;
