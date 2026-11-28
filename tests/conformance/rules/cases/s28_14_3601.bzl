//! case: s28_14_3601
//! doc: spec
//! section: 28.14
//! title: a scoped construction of this into an owned field of this reaches the this arm
//! rule: **`this`** (per forms 3/4, or through the initializer resolution of form 1 where the
//!   constructor call is directly assigned to (or initializes) an `owned` field of `this` — form 1
//!   composing with form 3's placement as it composes with form 2's above: `_child := new Child(scoped
//!   this)` in a member of the owning class)
//! cites: 28.14
//! expect: run
//! stdout: Root
//! issue: 8397
//! notes: The issue's repro. Rejecting twin s28_14_3603 passes a constructor parameter instead of `this`.
module Conf;

public class Owner
  private _name: string;
  private owned _child: Child?;
  public constructor(name: string)
    _name := name;
    _child := new Child(scoped this);
  end constructor;
  public property Name: string => _name;
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
    using var o := new Owner("Root");
    Console.WriteLine(o.ChildOwner());
    return 0;
  end Main;
end Program;
