//! case: s28_7_0005
//! doc: spec
//! section: 28.7
//! title: reassigning an owned field while a live borrow of the displaced value exists is admitted
//! rule: Reassigning an owned field or collection slot is not statically
//!   frozen — a borrow of the displaced value is invalidated at the
//!   overwrite, caught as DisposedAccessException in checked disposal mode
//!   and undefined unchecked, not rejected at compile time.
//! cites: 28.7, 28.3
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method Ping()
  end Ping;
end Widget;

public class Holder
  private owned _w: Widget;

  public constructor()
    _w := new Widget();
  end constructor;

  public method Replace()
    var b := _w;
    _w := new Widget();
    b.Ping();
  end Replace;
end Holder;
