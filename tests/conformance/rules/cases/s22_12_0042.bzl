//! case: s22_12_0042
//! doc: spec
//! section: 22.12
//! title: the same static owned readonly field at a class outside the set warns
//! rule: A `static readonly` (including `owned`) field or auto-property of **class
//!   type** is admitted with a **declaration-site warning** unless the class is in
//!   this rule's warning-suppression set: confining its use to one domain is the
//!   programmer's obligation.
//! cites: 22.12, 30.14
//! expect: warn
//! diagnostic: one domain
module Conf;

public class Gate
  public constructor()
  end constructor;

  public method Enter()
    Console.WriteLine("entered");
  end Enter;

  destructor
    Console.WriteLine("gate closed");
  end destructor;
end Gate;

public static class Gates
  private static owned readonly _gate: Gate := new Gate();

  public static method Enter()
    _gate.Enter();
  end Enter;
end Gates;
