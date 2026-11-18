//! case: s28_2_6719
//! doc: spec
//! section: 28.2
//! title: a read after a direct DisposeAsync through the bang is an error
//! rule: A use of a local variable — a read, a borrow taken from it, a member access, an argument position, a consume — after an explicit `Dispose()`
//! cites: 28.3, 28.12
//! expect: reject
//! diagnostic: dominat
module Conf;

public sealed class Ar: IAsyncDisposable
  private _id: int;
  public constructor(id: int)
    _id := id;
  end constructor;
  public method Use(): int => _id;
  public async method DisposeAsync(): Task
  end DisposeAsync;
end Ar;

public class Program
  public static async method Body(): Task<int>
    var a: Ar? := new Ar(1);
    await a!.DisposeAsync();
    return a!.Use();
  end Body;
  public static async method Main(): Task
    Console.WriteLine(await Program.Body());
  end Main;
end Program;
