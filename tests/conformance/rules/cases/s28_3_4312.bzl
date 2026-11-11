//! case: s28_3_4312
//! doc: spec
//! section: 28.3
//! title: a plain Task local read after it was stored into an owned Task field is a compile error
//! rule: **moved-from** — a later read is an error until reassigned
//! cites: 28.3.1
//! expect: reject
//! diagnostic: already moved
module Conf;

public class Holder
  public owned T: Task? := null;
  public constructor() end constructor;
end Holder;

public class Program
  public static async method W(): Task
    await Task.Delay(1);
  end W;

  public static async method Main(): Task
    using var h := new Holder();
    var t := Program.W();
    h.T := t;
    await t;
    Console.WriteLine("END");
  end Main;
end Program;
