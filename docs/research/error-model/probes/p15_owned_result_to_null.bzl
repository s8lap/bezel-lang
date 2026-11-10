// Probe p15: the "to null" stance over an OWNED result. `OpenOrNull(name)`
// stands for `(try new Conn(name))`: a helper that returns null where the
// constructor raises a failure (an exception whose class implements the
// marker interface, as in p16) and lets anything else through. The nullable
// owned result binds to `using var`, narrows, and is disposed on every path
// with today's rules (spec §28.10, §7.4, §31.1). Other positions: p19.
module P15;

public interface IFailure
end IFailure;

public class ConnectFailure: Exception, IFailure
  public constructor(msg: string) base(msg);
  end constructor;
end ConnectFailure;

public class Conn
  private _name: string;
  public constructor(name: string)
    if name.Length = 0 then
      throw new ConnectFailure("no such host");
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
      IFailure => return null;
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
