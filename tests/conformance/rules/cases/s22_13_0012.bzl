//! case: s22_13_0012
//! doc: spec
//! section: 22.13
//! title: static owned fields of one type are disposed at program exit in reverse declaration order
//! rule: Within a single type, multiple static `owned` fields are
//!   disposed in reverse declaration order, mirroring the instance-field
//!   disposal rule.
//! cites: 22.13, 28.4
//! expect: run
//! stdout: second
module Conf;

public class Resource
  private readonly _name: string;

  public constructor(name: string)
    _name := name;
  end constructor;

  destructor
    Console.WriteLine(_name);
  end destructor;
end Resource;

public static class Holder
  private static owned readonly _first: Resource := new Resource("first");
  private static owned readonly _second: Resource := new Resource("second");

  public static method Touch()
  end Touch;
end Holder;

public class Program
  public static method Main()
    Holder.Touch();
  end Main;
end Program;
