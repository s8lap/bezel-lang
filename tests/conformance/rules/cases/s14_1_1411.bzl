//! case: s14_1_1411
//! doc: spec
//! section: 14.1
//! title: an async method of Box<Guid> reading another class's Guid member keeps the Guid type
//! rule: Type parameters of declarations that do not nest share no scope
//! cites: 14.1, 2.7
//! expect: run
//! stdout: 36
module Conf;
public class Holder
  public property G: Guid;
  public constructor()
    G := Guid.NewGuid();
  end constructor;
end Holder;
public class Box<Guid>
  public async method Go(h: Holder): Task<int>
    await Task.Delay(0);
    return h.G.ToString().Length;
  end Go;
end Box;
public class Program
  public static async method Main(): Task<int>
    using var h := new Holder();
    using var b := new Box<int>();
    var n := await b.Go(h);
    Console.WriteLine(n);
    return 0;
  end Main;
end Program;
