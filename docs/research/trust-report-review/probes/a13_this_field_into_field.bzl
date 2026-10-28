module Conf;
// Spelling twin of a04: `_alias := this._child;` (member-access spelling of the same receiver-rooted field path). Compare with a04's bare `_child`.
public class Child
  public constructor()
  end constructor;
end Child;
public class Owner
  private owned _child: Child;
  private _alias: Child;
  public constructor()
    _child := new Child();
    _alias := this._child;
  end constructor;
end Owner;
