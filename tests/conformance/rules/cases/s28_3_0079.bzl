//! case: s28_3_0079
//! doc: spec
//! section: 28.3
//! title: an undischarged owned parameter draws the must-use warning at its declaration
//! rule: The rule is uniform across body kinds: the must-use warning attaches at
//!   the implicit-drop site of a plain-`var` owned local **or `owned` parameter**
//!   that is not trivial-dispose.
//! cites: 28.3, 22.3
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
  public method Consume(owned h: Handle)
    h.Touch();
  end Consume;
end Session;
