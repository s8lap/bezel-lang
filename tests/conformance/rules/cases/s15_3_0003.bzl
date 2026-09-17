//! case: s15_3_0003
//! doc: spec
//! section: 15.3
//! title: a move-only value carrying no borrows may be a field of a class
//! rule: A move-only value carrying no borrows may be a field of a class; the
//!   class's destructor disposes the field's owned slots (§15.3).
//! cites: 15.3
//! expect: accept
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public class Pool
  private _slots: [2 owned]Connection;

  public constructor()
    _slots := [new Connection(), new Connection()];
  end constructor;
end Pool;
