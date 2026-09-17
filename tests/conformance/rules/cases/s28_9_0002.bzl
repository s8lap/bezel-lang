//! case: s28_9_0002
//! doc: spec
//! section: 28.9
//! title: a closure that would outlive its captured local borrow's source is rejected
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

public class Program
  public method Make(): () -> ()
    var w := new Widget();
    return lambda() => w.Ping();
  end Make;
end Program;
