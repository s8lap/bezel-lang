//! case: s28_3_0071
//! doc: spec
//! section: 28.3
//! title: an undischarged plain-var owned local in a library method draws the must-use warning
//! rule: At the implicit-drop site of a plain-`var` owned local or an
//!   undischarged `owned` parameter that is not trivial-dispose under §28.9's
//!   ceremony exemption ... the compiler emits a **warning** (not an error):
//!   a plain `var` that is implicitly dropped is indistinguishable in
//!   *effect* from a `using var`, so the drop may have been unintended
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
