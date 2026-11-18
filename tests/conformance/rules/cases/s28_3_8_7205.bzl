//! case: s28_3_8_7205
//! doc: spec
//! section: 28.3.8
//! title: a capture-bearing function value as receiver of a member Object
//!   declares is admitted
//! rule: A function value as the receiver of a member `Object` declares
//!   (`f.ToString()`, `f.GetHashCode()`, an interpolation hole) is not at a
//!   set-erasing position
//! cites: 28.3, 8.1
//! expect: accept
module Conf;

public class Res
  public constructor()
  end constructor;

  public method Use(): int => 3;

  public static method Stat(): int => 5;
end Res;

public extension Object
  public method Hold(): int => 1;
end extension;

public class Client
  public static method Run(payload: Res)
    var f: () -> int := lambda() => payload.Use();
    var s := f.ToString();
    var h := f.GetHashCode();
  end Run;
end Client;
