//! case: s2_2_0005
//! doc: spec
//! section: 2.2
//! title: a current-module declaration shadows a same-named auto-imported name
//! rule: A declaration in the current module shadows any same-named import
//!   for bare-name purposes (the import stays reachable by qualifier) — the
//!   same shadowing already specified for auto-imported names.
//! cites: 2.2, 2.3
//! expect: run
//! stdout: qualified-ok
module Conf;

public class Console
  public static method Announce()
  end Announce;
end Console;

public class Program
  public static method Main()
    Console.Announce();
    System.Console.WriteLine("qualified-ok");
  end Main;
end Program;
