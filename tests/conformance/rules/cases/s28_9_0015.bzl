//! case: s28_9_0015
//! doc: spec
//! section: 28.9
//! title: owned over the Type token is inadmissible
//! rule: `Type` is runtime-managed: no `Type` expression is an owned
//!   production, none lends a borrow, and no holder carries an obligation for
//!   one — `owned` over `Type` is inadmissible (§28.9, v0.300; §28.1's type
//!   requirement names a class type other than `Type`).
//! cites: 28.9, 28.1, 28.5
//! expect: reject
//! diagnostic: owned
module Conf;

public class Registry
  public constructor()
  end constructor;

  public method Note(owned t: Type)
    Console.WriteLine(t.Name);
  end Note;
end Registry;
