//! case: s23_4_0001
//! doc: spec
//! section: 23.4
//! title: an expression-bodied indexer is admitted
//! rule: An indexer is a property whose name is replaced by a parameter list in
//!   square brackets.
//! cites: 23.4
//! expect: accept
module Conf;

public class Names
  private owned _items: Array<string> := new Array<string>();

  public constructor()
  end constructor;

  public property [i: int]: string => _items[i];
end Names;
