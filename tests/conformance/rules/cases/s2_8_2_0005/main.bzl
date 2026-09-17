//! case: s2_8_2_0005
//! doc: spec
//! section: 2.8.2
//! title: a public generic instantiates at a consumer-declared type argument
//! rule: The artifact interface must be sufficient for a consumer to
//!   instantiate every public generic type at arbitrary module-external type
//!   arguments satisfying the declared constraints; an external instantiation
//!   behaves identically to a same-module instantiation at the same arguments.
//! cites: 2.8.2, 14.2
//! expect: run
//! stdout: base
module App;
import Lib;

public class Local: Base
  public constructor()
  end constructor;
end Local;

public class Program
  public static method Main()
    using var item := new Local();
    using var holder := new Holder<Local>();
    Console.WriteLine(holder.Describe(item));
  end Main;
end Program;
