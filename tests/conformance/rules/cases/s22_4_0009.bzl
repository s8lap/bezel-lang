//! case: s22_4_0009
//! doc: spec
//! section: 22.4
//! title: a pre-declared uninitialized local adopts owned against out owned
//! rule: A pre-declared local passed as plain `out x` is admitted against
//!   `out owned` exactly when it is in one of the two adoptable states —
//!   uninitialized or polymorphic-null — and adopts owned on every
//!   normal-return path of the call.
//! cites: 22.4, 28.3
//! expect: accept
module Conf;

public class Conn
  public constructor()
  end constructor;
end Conn;

public class Factory
  public static method TryCreate(out owned c: Conn?): bool
    c := new Conn();
    return true;
  end TryCreate;
end Factory;

public class Runner
  public static method Run()
    var c: Conn?;
    if Factory.TryCreate(out c) then
      using var taken := move c;
    end if;
  end Run;
end Runner;
