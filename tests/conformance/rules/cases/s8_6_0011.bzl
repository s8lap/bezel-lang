//! case: s8_6_0011
//! doc: spec
//! section: 8.6
//! title: a record holding no borrow boxes as the receiver of an extension on a user interface
//! rule: When a value type is used where `Object` (or an interface implemented by
//!   the value type) is expected, the value is boxed: a heap-allocated
//!   wrapper holds the value.
//! cites: 8.6, 26.5
//! expect: run
//! stdout: 1
module Conf;

public interface IShow
  method Name(): string;
end IShow;

public class Keeper
  private _kept: IShow?;

  public constructor()
  end constructor;

  public method Put(s: IShow)
    _kept := s;
  end Put;

  public method Count(): int => (if _kept <> null then 1 else 0);
end Keeper;

public extension IShow
  public method Leak(k: Keeper) => k.Put(this);
end extension;

public record Plain(N: int): IShow
  public method Name(): string => "plain";
end Plain;

public class Program
  public static method Main()
    using var k := new Keeper();
    var p := new Plain(3);
    p.Leak(k);
    Console.WriteLine(k.Count());
  end Main;
end Program;
