//! case: l5_0035
//! doc: stdlib
//! section: 5
//! title: a user interface named IList shadows the library interface and its own members resolve
//! rule: Auto-imported names may be shadowed by a declaration in the module (spec 2.3); on a receiver of the user's own IList<string> its instance members First, ToArray, Contains and Sum are the ones called.
//! cites: 2.3, 26.5
//! expect: run
//! output: 42
//!   7
//!   True
//!   9
module Conf;

public interface IList<T>
  method First(): int;
  method ToArray(): int;
  method Contains(x: T): bool;
  method Sum(): int;
end IList;

public class Impl: IList<string>
  public method First(): int => 42;
  public method ToArray(): int => 7;
  public method Contains(x: string): bool => true;
  public method Sum(): int => 9;
end Impl;

public class Program
  public static method Main()
    var i := new Impl();
    var l: IList<string> := i;
    Console.WriteLine(l.First());
    Console.WriteLine(l.ToArray());
    Console.WriteLine(l.Contains("a"));
    Console.WriteLine(l.Sum());
  end Main;
end Program;
