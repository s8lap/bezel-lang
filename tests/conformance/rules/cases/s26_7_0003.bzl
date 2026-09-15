//! case: s26_7_0003
//! doc: spec
//! section: 26.7
//! title: extensions may return owned values
//! rule: Extensions may return `owned` values.
//! cites: 26.7
//! expect: run
//! stdout: fresh
module Conf;

public class Token
  public constructor()
  end constructor;

  public method Tag(): string => "fresh";
end Token;

public class Widget
  public constructor()
  end constructor;
end Widget;

public extension Widget
  public method MintToken(): owned Token => new Token();
end;

public class Program
  public static method Main()
    using var w := new Widget();
    using var t := w.MintToken();
    Console.WriteLine(t.Tag());
  end Main;
end Program;
