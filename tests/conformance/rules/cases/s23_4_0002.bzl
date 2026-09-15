//! case: s23_4_0002
//! doc: spec
//! section: 23.4
//! title: a block-form indexer closes with a bare end
//! rule: A block-form indexer's `end` clause has no name to reference; it is plain
//!   `end;`.
//! cites: 23.4, 1.6
//! expect: accept
module Conf;

public class Names
  private owned _items: Array<string> := new Array<string>();

  public constructor()
  end constructor;

  public property [i: int]: string
    get => _items[i];
    set => do _items[i] := value; end;
  end;
end Names;
