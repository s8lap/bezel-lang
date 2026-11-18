//! case: s28_14_7522
//! doc: spec
//! section: 28.14
//! title: a scoped this borrow stored into an owned field of ANOTHER object is rejected
//! rule: A closure argument capturing locals or parameters is admitted iff the bounding object resolves
//!   to one of:
//! cites: 28.14
//! expect: reject
//! diagnostic: scoped
module Conf;
// the scoped-this child assigned to an owned field of ANOTHER object (not this): must reject.
public class Owner
  private _name: string;
  private owned _child: Child?;
  public constructor(name: string)
    _name := name;
  end constructor;
  public property Name: string => _name;
  public method Adopt(other: Owner)
    other._child := new Child(scoped this);
  end Adopt;
  public method ChildOwner(): string => _child!.OwnerName();
end Owner;
public class Child
  private _owner: Owner;
  public constructor(scoped owner: Owner)
    _owner := owner;
  end constructor;
  public method OwnerName(): string => _owner.Name;
end Child;
public class Program
  public static method Main(): int
    using var keeper := new Owner("keeper");
    do
      using var temp := new Owner("temp");
      temp.Adopt(keeper);
    end;
    Console.WriteLine(keeper.ChildOwner());
    return 0;
  end Main;
end Program;
