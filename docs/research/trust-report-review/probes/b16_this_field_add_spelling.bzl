module Conf;
// Spelling twin of b02's missed line: `_peers.Add(this._owner)` vs the bare `_peers.Add(_owner)`. EXPECT: both report (field-path-sourced element).
public class Node
  private _owner: Node;
  private owned _peers: Array<Node>;
  public constructor(owner: Node)
    _owner := owner;
    _peers := new Array<Node>();
  end constructor;
  public method Link()
    _peers.Add(_owner);
    _peers.Add(this._owner);
  end Link;
end Node;
