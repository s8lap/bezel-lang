//! case: s2_8_4_0002
//! doc: spec
//! section: 2.8.4
//! title: a qualifying Main in a library compilation is an ordinary method
//! rule: A library build has no entry point; a qualifying `Main` in a library
//!   compilation is an ordinary method — it is declared, compiled, and
//!   callable like any other static method.
//! cites: 2.8.4, 33.1, 33.2
//! notes: dep-main
//! expect: run
//! stdout: 5
module App;
import Dep;

public class Program
  public static method Main()
    Console.WriteLine(Entry.Run());
  end Main;
end Program;
