//! case: s22_4_0008
//! doc: spec
//! section: 22.4
//! title: reading a non-nullable-reference out variable off the guarded path is rejected
//! rule: On a path where the call may not have run, a read of a
//!   non-nullable-reference-typed out variable is a compile error, exactly as
//!   for any other unassigned non-nullable local.
//! cites: 22.4
//! expect: reject
//! diagnostic: definitely assigned
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Cache
  private owned _w: Widget := new Widget();

  public constructor()
  end constructor;

  public method TryGet(flag: bool, out w: Widget): bool
    w := _w;
    return flag;
  end TryGet;
end Cache;

public class Program
  public static method Main()
    using var cache := new Cache();
    var flag := true;
    if flag and cache.TryGet(flag, out var w) then
    end if;
    Console.WriteLine(w);
  end Main;
end Program;
