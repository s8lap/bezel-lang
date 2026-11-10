//! case: s28_10_4204
//! doc: spec
//! section: 28.10
//! title: an unmarked using var over a borrowing alias of a Task field is a pin and is admitted
//! rule: the `using var` does not assume a disposal obligation
//! cites: 28.10, 28.3
//! expect: accept
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

  public static async method Run(): Task
    using var h := new Holder();
    h.Set(Program.W());
    var t := h.T!;
    using var u := t;
  end Run;
end Program;
