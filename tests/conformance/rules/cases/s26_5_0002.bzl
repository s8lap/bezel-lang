//! case: s26_5_0002
//! doc: spec
//! section: 26.5
//! title: a failed instance lookup does not fall back to an extension
//! rule: If the committed instance lookup fails — no applicable overload — the call
//!   is a compile error citing the instance members; it does not fall back to an
//!   extension.
//! cites: 26.5
//! expect: reject
//! diagnostic: Describe
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method Describe(n: int): string => "instance";
end Widget;

extension Widget
  public method Describe(): string => "extension";
end;

public class Program
  public static method Main()
    using var w := new Widget();
    Console.WriteLine(w.Describe());
  end Main;
end Program;
