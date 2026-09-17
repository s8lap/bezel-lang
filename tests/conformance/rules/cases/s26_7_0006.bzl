//! case: s26_7_0006
//! doc: spec
//! section: 26.7
//! title: other parameters may be owned in an extension, following normal
//!   method rules
//! rule: Other parameters may be `owned`, following normal method rules.
//! cites: 26.7
//! expect: run
//! stdout: consumed
module Conf;

public class Token
  public constructor()
  end constructor;

  public method Tag(): string => "consumed";
end Token;

public class Widget
  public constructor()
  end constructor;
end Widget;

public extension Widget
  public method Absorb(owned t: Token): string
    using var held := move t;
    return held.Tag();
  end Absorb;
end;

public class Program
  public static method Main()
    using var w := new Widget();
    using var tok := new Token();
    Console.WriteLine(w.Absorb(move tok));
  end Main;
end Program;
