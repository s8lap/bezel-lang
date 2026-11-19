//! case: s28_14_7712
//! doc: spec
//! section: 28.14
//! title: a call-bounded function parameter may not be stored in a field
//! rule: It may not be stored in a field of any object
//! cites: 28.3
//! expect: reject
//! diagnostic: call-bounded
module Conf;
public class H
  private _cb: (() -> int)? := null;
  public method Hold(g: () -> int)
    this._cb := g;
  end Hold;
end H;
