//! case: s28_5_0021
//! doc: spec
//! section: 28.5
//! title: a generic owned field instantiated at an owning choice is admitted and disposes the payload once
//! rule: When a generic class declares `private owned _slot: T` (where T is a
//!   type parameter), the restrictions above are checked at each
//!   instantiation, reading the substituted type as they read a declared
//!   one ... an owning-choice substitution ... is admitted-but-redundant,
//!   exactly as the first bullet reads the direct field
//! cites: 28.5, 35.4, 15.3
//! expect: run
//! output: closed
//!   after
module Conf;

public class Res
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("closed");
  end destructor;
end Res;

public choice Slot
  Held(owned r: Res),
  Empty;
end Slot;

public class Box<T>
  private owned _slot: T;

  public constructor(owned slot: T)
    _slot := slot;
  end constructor;
end Box;

public class Program
  public static method Main()
    do
      using var b := new Box<Slot>(Slot.Held(new Res()));
    end;
    Console.WriteLine("after");
  end Main;
end Program;
