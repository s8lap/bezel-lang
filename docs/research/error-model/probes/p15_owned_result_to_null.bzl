// Probe p15: the "to null" stance over an OWNED result, written by hand as the
// desugaring a call-site form would produce: try { return Open() } catch
// IOException => null. The nullable owned result binds to `using var`,
// narrows, and is disposed on every path with today's rules — no new
// ownership rule is involved (spec §28.10, §7.4, §31.1).
module P15;

import System.IO;

public class Conn
  private _name: string;
  public constructor(name: string)
    if name.Length = 0 then
      throw new IOException("no such host");
    end if;
    _name := name;
    Console.WriteLine($"  open {name}");
  end constructor;
  public method Send(s: string) => Console.WriteLine($"  {_name} <- {s}");
  destructor
    Console.WriteLine($"  close {_name}");
  end destructor;
end Conn;

public class Program
  // What `try? new Conn(name)` would mean, spelled out.
  public static method OpenOrNull(name: string): owned Conn?
    try
      return new Conn(name);
    catch
      IOException => return null;
    end try;
  end OpenOrNull;

  public static method Use(name: string)
    using var c := Program.OpenOrNull(name);
    if c = null then
      Console.WriteLine("  unreachable, falling back");
      return;
    end if;
    c.Send("hello");
  end Use;

  public static method Main()
    Console.WriteLine("good host:");
    Program.Use("db");
    Console.WriteLine("bad host:");
    Program.Use("");
  end Main;
end Program;
