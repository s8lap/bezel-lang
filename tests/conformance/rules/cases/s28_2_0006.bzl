//! case: s28_2_0006
//! doc: spec
//! section: 28.2
//! title: an explicit Dispose discharges the local and needs no further discharge
//! rule: The program may discharge an owned local's obligation explicitly — disposing
//!   it by hand at a consume-table position — after which the scope-end pass skips
//!   it; a further discharge is neither required nor written.
//! cites: 28.3
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Demo()
    var w := new Widget();
    w.Dispose();
  end Demo;
end Program;
