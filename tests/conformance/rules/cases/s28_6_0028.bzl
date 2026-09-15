//! case: s28_6_0028
//! doc: spec
//! section: 28.6
//! title: an owned field passed as a plain argument may be stored as a back-reference borrow
//! rule: Passing an owned field as a plain argument is permitted; the callee
//!   receives a parameter-sourced borrow and may store the borrow into a
//!   non-owned class-typed field only where the borrowed-field store rule
//!   admits it (the back-reference idiom).
//! cites: 28.3, 28.5, 28.6
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Logger
  private _target: Widget;

  public constructor(target: Widget)
    _target := target;
  end constructor;
end Logger;

public class Pool
  private owned _w: Widget;

  public constructor()
    _w := new Widget();
  end constructor;

  public method Attach(): owned Logger => new Logger(_w);
end Pool;
