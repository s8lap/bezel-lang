//! case: s8_6_0010
//! doc: spec
//! section: 8.6
//! title: a borrow-bearing record as the receiver of an extension on a user interface is rejected
//! rule: The receiver of an extension-block call whose head is `Object` or
//!   an interface type is not admitted: that body reads `this` at the
//!   head type, which announces no borrow, and may pass it to a parameter
//!   that stores it (§28.3's field-store bullet), so the receiver position is
//!   borrow-erasing like a parameter of that type.
//! cites: 8.6, 9.7, 26.5
//! expect: reject
//! diagnostic: borrow-erasing
//! issue: 5659
module Conf;

public class Item
  public constructor()
  end constructor;

  public method Size(): int => 1;
end Item;

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

public record Tagged(Src: Item): IShow
  public method Name(): string => "tagged";
end Tagged;

public class Client
  public static method Run(k: Keeper)
    using var it := new Item();
    var t := new Tagged(it);
    t.Leak(k);
  end Run;
end Client;
