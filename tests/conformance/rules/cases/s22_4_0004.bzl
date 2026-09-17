//! case: s22_4_0004
//! doc: spec
//! section: 22.4
//! title: out using var is accepted against an out owned parameter
//! rule: For disposal-aware inline declarations against an `out owned`
//!   parameter, use `out using var`.
//! cites: 22.4
//! expect: run
//! stdout: created
module Conf;

public class Conn
  public constructor()
  end constructor;
end Conn;

public class Factory
  public static method TryCreate(out owned c: Conn): bool
    c := new Conn();
    return true;
  end TryCreate;
end Factory;

public class Program
  public static method Main()
    if Factory.TryCreate(out using var c) then
      Console.WriteLine("created");
    end if;
  end Main;
end Program;
