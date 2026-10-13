//! case: s19_3_0069
//! doc: spec
//! section: 19.3
//! title: a public field named through a module-qualified alias head over an Object scrutinee is accepted
//! rule: Resolution honours ordinary member visibility (13). A public field of a record is named by the slot identifier through a module-qualified alias head, and the pattern matches the record's value when the scrutinee is an Object holding it.
//! cites: 19.3, 13.1
//! expect: run
//! stdout: 8
//! issue: 6045
module Conf;

public record PubRec
  public h: int;

  public constructor(x: int)
    h := x;
  end constructor;
end PubRec;

type R3 := PubRec;

public class Program
  public static method Main()
    var o: Object := new PubRec(8);
    case o of Conf.R3(h = var v) => Console.WriteLine(v.ToString());,
      _ => Console.WriteLine("n");
    end case;
  end Main;
end Program;
