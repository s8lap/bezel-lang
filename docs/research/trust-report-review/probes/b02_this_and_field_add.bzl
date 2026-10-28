module Conf;
// Family B: receiver-sourced (`this`) and field-sourced (`_owner`) elements stored into a field-held borrowing collection. EXPECT: two reports.
public class Node
  private _owner: Node;
  private owned _peers: Array<Node>;
  public constructor(owner: Node)
    _owner := owner;
    _peers := new Array<Node>();
  end constructor;
  public method Link()
    _peers.Add(this);
    _peers.Add(_owner);
  end Link;
end Node;
