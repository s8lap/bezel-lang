//! case: s7_10_0007
//! doc: spec
//! section: 7.10
//! title: a generic body's copy out of an as result is rejected at a move-only instantiation
//! rule: At a move-only substitution ... an operation the body writes as a copy
//!   of a `T`-typed value ... is **rejected at the instantiation** where its
//!   operand is held in the **borrowed** state: ... the result of `x as T` ...
//!   So over `class Hold<T>` with a field `_f: T?` and `method Take(v: T)`
//!   whose body binds `var r := v as T;` and stores `_f := r;`, `Hold<Slot>`
//!   is rejected at that store wherever it is named
//! cites: 7.10, 15.3, 3.6
//! expect: reject
//! diagnostic: move-only
//! issue: 5819
module Conf;

public class Conn
  public constructor()
  end constructor;
end Conn;

public choice Slot
  Full(owned c: Conn),
  Empty;
end Slot;

public class Hold<T>
  private _f: T?;

  public constructor()
    _f := null;
  end constructor;

  public method Take(v: T)
    var r := v as T;
    _f := r;
  end Take;
end Hold;

public class User
  public static method Run(h: Hold<Slot>): int => 1;
end User;
