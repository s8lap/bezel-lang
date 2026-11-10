// Probe p19: the positions the `(try E)` atom would have to work in, written
// with today's forms. `OpenOrNull(name)` stands for `(try new Conn(name))`:
// a call that yields an owned, nullable, unbound temporary.
//   (1) the statement reading: an outer owned local assigned inside the
//       protected block (the shape README §8.1 gives as an equivalence);
//   (2) the temporary under `??`;
//   (3) the temporary under `?.` and under `!`: spec K8.1 disposes an unbound
//       owned temporary at the end of its full expression. r40 does not: no
//       "close" is printed for the good host in (3) or (4), where a
//       non-nullable owned receiver in (5) is closed. A compiler gap, and the
//       one position of the atom that today's compiler does not support.
module P19;

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
  public static method OpenOrNull(name: string): owned Conn?
    try
      return new Conn(name);
    catch
      IFailure => return null;
    end try;
  end OpenOrNull;

  public static method OuterLocal(name: string)
    var c: Conn? := null;
    try
      c := new Conn(name);
    catch
      IFailure => ()
    end try;
    if c <> null then
      c.Send("outer local");
    end if;
  end OuterLocal;

  public static method Coalesce(name: string)
    using var c := Program.OpenOrNull(name) ?? new Conn("fallback");
    c.Send("coalesced");
  end Coalesce;

  public static method Chain(name: string)
    Program.OpenOrNull(name)?.Send("temporary");
    Console.WriteLine("  after the statement");
  end Chain;

  public static method Bang(name: string)
    Program.OpenOrNull(name)!.Send("asserted");
    Console.WriteLine("  after the statement");
  end Bang;

  public static method Open(name: string): owned Conn
    return new Conn(name);
  end Open;

  public static method Main()
    Console.WriteLine("(1) outer local, good host:");
    Program.OuterLocal("db");
    Console.WriteLine("(1) outer local, bad host:");
    Program.OuterLocal("");
    Console.WriteLine("(2) ??, good host:");
    Program.Coalesce("db");
    Console.WriteLine("(2) ??, bad host:");
    Program.Coalesce("");
    Console.WriteLine("(3) ?., good host:");
    Program.Chain("db");
    Console.WriteLine("(3) ?., bad host:");
    Program.Chain("");
    Console.WriteLine("(4) !, good host:");
    Program.Bang("db");
    Console.WriteLine("(5) control, non-nullable owned receiver:");
    Program.Open("db").Send("control");
    Console.WriteLine("  after the statement");
  end Main;
end Program;
