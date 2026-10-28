module Conf;
// Family A: a receiver-rooted field path (`this._child`) stored into another plain field of this. EXPECT: report (receiver-sourced).
public class Child
  public constructor()
  end constructor;
end Child;
public class Owner
  private owned _child: Child;
  private _alias: Child;
  public constructor()
    _child := new Child();
    _alias := _child;
  end constructor;
end Owner;
