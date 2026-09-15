//! case: s28_9_0002
//! doc: spec
//! section: 28.9
//! title: a closure capturing a scope-bound borrow that would outlive its source is rejected
//! rule: Runtime management relaxes nothing about the values a closure
//!   captures — a captured borrowed class reference is subject to §28.3's
//!   borrow-escape rules, so a function value that outlives the captured
//!   borrow's source is rejected.
//! cites: 28.9, 28.3
//! expect: reject
//! diagnostic: outlive
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method Ping()
  end Ping;
end Widget;

public class Registry
  private _cb: () -> ();

  public method Register(cb: () -> ())
    _cb := cb;
  end Register;
end Registry;

public class Program
  public method Wire(reg: Registry)
    var w := new Widget();
    reg.Register(lambda() => w.Ping());
  end Wire;
end Program;
