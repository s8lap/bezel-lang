//! case: s11_6_0003
//! doc: spec
//! section: 11.6
//! title: a nested type's qualified name includes the enclosing path
//! rule: A nested type is referenced from outside as Outer.Nested, and the
//!   enclosing path is part of its qualified name wherever one is rendered.
//! cites: 11.6
//! expect: run
//! stdout: Conf.Parser.Token
module Conf;

public class Parser
  public class Token
    public constructor()
    end constructor;
  end Token;

  public constructor()
  end constructor;
end Parser;

public class Program
  public static method Main()
    using var t := new Parser.Token();
    Console.WriteLine(t);
  end Main;
end Program;
