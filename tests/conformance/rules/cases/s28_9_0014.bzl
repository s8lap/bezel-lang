//! case: s28_9_0014
//! doc: spec
//! section: 28.9
//! title: a borrowing collection is trivial-dispose even at a non-trivial element type
//! rule: The rule does not require the type arguments to be themselves
//!   trivial-dispose: `Array<MyClass>` is trivial-dispose even if `MyClass` is
//!   not — the elements are owned elsewhere and the array's free does not
//!   dispose them — so its undischarged local draws no must-use warning
//!   (§28.9).
//! cites: 28.9, 28.11, 28.3
//! expect: quiet
module Conf;

public class Resource
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("released");
  end destructor;
end Resource;

public class Program
  public static method Run()
    var snapshot := new Array<Resource>();
    Console.WriteLine(snapshot.Count);
  end Run;
end Program;
