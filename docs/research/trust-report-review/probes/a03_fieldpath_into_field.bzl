module Conf;
// Family A: a FIELD-PATH-sourced borrow (`owner.Child`, a property read through a parameter) stored into a plain field.
// §28.3 clause 3 "directly, through a field path rooted at it (`this.keep := x.child`)". EXPECT: report.
public class Child
  public constructor()
  end constructor;
end Child;
public class Owner
  private owned _child: Child;
  public constructor()
    _child := new Child();
  end constructor;
  public property Child: Child => _child;
end Owner;
public class Keeper
  private _keep: Child;
  public constructor(owner: Owner)
    _keep := owner.Child;
  end constructor;
end Keeper;
