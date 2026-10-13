//! case: s22_12_0052
//! doc: spec
//! section: 22.12
//! title: a static readonly bare-parameter field named at an OwningTask instantiation is rejected there
//! rule: a class-type substitution takes this rule's warning unless the
//!   substituted class is in the warning-suppression set, an `OwningTask<X>`
//!   substitution excepted, rejected on (j)'s single-award ground ... The
//!   diagnostic lands **at the instantiation site** and names the offending
//!   type argument
//! cites: 22.12, 14.1, 3.6
//! expect: reject
//! diagnostic: OwningTask
//! issue: 5899
module Conf;

public class Res
  public constructor()
  end constructor;
end Res;

public class Cell<T>
  public static readonly V: T?;
end Cell;

public class Client
  public static method Probe(): bool
    return Cell<OwningTask<Res>>.V = null;
  end Probe;
end Client;
