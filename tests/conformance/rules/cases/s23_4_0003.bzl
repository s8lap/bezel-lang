//! case: s23_4_0003
//! doc: spec
//! section: 23.4
//! title: a block-form indexer closed with an identifier label is rejected
//! rule: An `end`-label that is supplied must match the construct it closes; an
//!   indexer has no name to reference, so a supplied identifier is a mismatch.
//! cites: 23.4, 1.6
//! expect: reject
//! diagnostic: no name to label
module Conf;

public class Names
  private owned _items: Array<string> := new Array<string>();

  public constructor()
  end constructor;

  public property [i: int]: string
    get => _items[i];
    set => do _items[i] := value; end;
  end Names;
end Names;
