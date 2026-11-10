// Probe p07 (expected: rejected). Proposal 0076 §6 condition 2: an async
// acquisition returning Result<resource, E> has no container. spec §35.4 /
// §30.5 (v0.355): a Task over an owning choice is a compile error.
module P07;

public class Connection
  public constructor() end constructor;
  destructor
    Console.WriteLine("close");
  end destructor;
end Connection;

public class Program
  public static async method Connect(host: string): Task<Result<Connection, string>>
    await Task.Delay(1);
    if host.Length = 0 then
      return Error("no host");
    end if;
    return Ok(new Connection());
  end Connect;

  public static async method Main(): Task
    case await Program.Connect("db") of
      Ok(_)        => Console.WriteLine("connected"),
      Error(var m) => Console.WriteLine(m)
    end case;
  end Main;
end Program;
