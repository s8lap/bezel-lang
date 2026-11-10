//! case: s28_3_4201
//! doc: spec
//! section: 28.3
//! title: the transfer marker on a borrowing alias of a Task field is rejected
//! rule: Three spelling errors follow, mirroring the call-site marker's rules: the marker on a borrow or fresh-producer right-hand side
//! cites: 28.3, 28.8.1, 28.10
//! expect: reject
//! diagnostic: on a borrow
//! issue: 7880
module Conf;

public class Holder
  public owned T: Task? := null;

  public method Set(owned x: Task)
    this.T := x;
  end Set;
end Holder;

public class Program
  public static async method W(): Task
    Console.WriteLine("w");
  end W;

  public static async method Main(): Task
    using var h := new Holder();
    h.Set(Program.W());
    var t := h.T!;
    using var u := move t;
  end Main;
end Program;
