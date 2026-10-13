//! case: s22_12_0053
//! doc: spec
//! section: 22.12
//! title: the same bare-parameter static named at Task of a class is admitted
//! rule: A `static readonly` field or auto-property whose declared type is a
//!   **bare generic parameter**, or its nullable form (`T`, `T?`), is admitted
//!   at the declaration and classified **per instantiation, after
//!   substitution**: ... a class-type substitution takes this rule's warning
//!   unless the substituted class is in the warning-suppression set
//! cites: 22.12, 14.1
//! expect: accept
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
    return Cell<Task<Res>>.V = null;
  end Probe;
end Client;
