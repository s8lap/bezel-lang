//! case: s35_4_0005
//! doc: spec
//! section: 35.4
//! title: drop-on-overwrite disposes the displaced owning-choice value at the store
//! rule: Drop-on-overwrite is defined and automatic: whole-value reassignment of an
//!   owning-choice local or field disposes the displaced value per §28.7; the RHS
//!   is evaluated first, then the displaced value is disposed, then the new value
//!   is stored.
//! cites: 35.4, 28.7, 15.3
//! expect: run
//! output: dispose old
//!   reassigned
//!   dispose new
module Conf;

public class Resource
  private _name: string;

  public constructor(name: string)
    _name := name;
  end constructor;

  destructor
    Console.WriteLine($"dispose {_name}");
  end destructor;
end Resource;

public choice Bundle
  Ready(owned r: Resource),
  Empty;
end Bundle;

public class Program
  public static method Main()
    var b := Bundle.Ready(new Resource("old"));
    b := Bundle.Ready(new Resource("new"));   // drop-on-overwrite disposes "old" here
    Console.WriteLine("reassigned");
    _ := b;                                   // disposes "new" at the end of this statement
  end Main;
end Program;
