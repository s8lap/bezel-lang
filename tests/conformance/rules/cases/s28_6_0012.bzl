//! case: s28_6_0012
//! doc: spec
//! section: 28.6
//! title: an object may not dispose itself through this
//! rule: The borrow receivers include a borrowed local or parameter —
//!   including `this`: an object may not dispose itself out from under its
//!   owner.
//! cites: 28.6
//! expect: reject
//! diagnostic: dispose itself
module Conf;

public class Session
  public constructor()
  end constructor;

  public method Close()
    this.Dispose();
  end Close;
end Session;
