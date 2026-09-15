//! case: s28_3_0071
//! doc: spec
//! section: 28.3
//! title: an undischarged plain-var owned local in a library method draws the must-use warning
//! rule: The compiler emits a warning at the implicit-drop site of a plain-`var`
//!   owned local that is not trivial-dispose under §28.9's ceremony exemption, to
//!   surface a drop that may have been unintended.
//! cites: 28.3, 28.9
//! expect: warn
//! diagnostic: must use
module Conf;

public class Handle
  public constructor()
  end constructor;

  public method Touch()
    Console.WriteLine("touched");
  end Touch;

  destructor
    Console.WriteLine("handle released");
  end destructor;
end Handle;

public class Session
  public method Work()
    var h := new Handle();
    h.Touch();
  end Work;
end Session;
