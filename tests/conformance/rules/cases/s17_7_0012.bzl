//! case: s17_7_0012
//! doc: spec
//! section: 17.7
//! title: a builtin record's constructor arity is checked
//! rule: **Standard-library builtins have a closed constructor surface.** For a standard-library builtin type — any type the standard-library document declares — the constructor surface is exactly what that document declares — `new T(...)` resolves only against a declared constructor, never against a compiler-supplied fallback.
//! cites: 17.7, 2.15, 22.5
//! expect: reject
//! diagnostic: declared constructor
//! issue: 7958
module Conf;
import System.Serialization;

public class Program
  public static method Main(): int
    var o := new JsonOptions(true, 4);
    Console.WriteLine(o.IndentWidth);
    return 0;
  end Main;
end Program;
