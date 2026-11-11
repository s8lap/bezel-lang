//! case: s28_3_4313
//! doc: spec
//! section: 28.3
//! title: an owned Task parameter read after it was stored into an owned Task field is a compile error
//! rule: **moved-from** — a later read is an error until reassigned
//! cites: 28.3.1, 28.1
//! expect: reject
//! diagnostic: already moved
module Conf;

public class Holder
  public owned T: Task? := null;
  public constructor() end constructor;

  public async method Put(owned x: Task): Task
    this.T := x;
    await x;
  end Put;
end Holder;

public class Program
  public static async method W(): Task
    await Task.Delay(1);
  end W;

  public static async method Main(): Task
    using var h := new Holder();
    await h.Put(Program.W());
    Console.WriteLine("END");
  end Main;
end Program;
