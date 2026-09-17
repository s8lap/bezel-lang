//! case: s28_13_0003
//! doc: spec
//! section: 28.13
//! title: a Shared<T> handle may be stored in an owning collection like any
//!   class instance
//! rule: A handle may be stored in owning collections
//!   (`OwningArray<Shared<T>>`) like any class instance.
//! cites: 28.13
//! expect: run
//! stdout: conn
module Conf;

public class Connection
  public constructor()
  end constructor;

  public method Tag(): string => "conn";
end Connection;

public class Program
  public static method Main()
    using var handles := new OwningArray<Shared<Connection>>();
    handles.Add(new Shared<Connection>(new Connection()));
    Console.WriteLine(handles[0].Get().Tag());
  end Main;
end Program;
