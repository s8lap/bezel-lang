//! case: s7_10_0008
//! doc: spec
//! section: 7.10
//! title: storing the parameter itself moves it at a move-only instantiation and is admitted
//! rule: At a move-only substitution ... an operation the body writes as a copy
//!   of a `T`-typed value is a move where its operand is a named owned binding
//!   the body may consume — a plain or `owned` parameter ... So over `class
//!   Hold<T>` ... `Hold<Slot>` is rejected at that store wherever it is named,
//!   while `_f := v;` moves the parameter and is admitted.
//! cites: 7.10, 15.3, 3.6
//! expect: accept
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
    _f := v;
  end Take;
end Hold;

public class User
  public static method Run(h: Hold<Slot>): int => 1;
end User;
