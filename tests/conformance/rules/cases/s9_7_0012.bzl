//! case: s9_7_0012
//! doc: spec
//! section: 9.7
//! title: a null-source borrow-bearing record value carries no borrow and may be returned
//! rule: A borrow-bearing record may not be returned from a method whose
//!   return-type rules cannot establish a sufficient lifetime for every borrow
//!   the record carries; a null-source value — every reference-typed field
//!   null — carries none.
//! cites: 28.3
//! expect: run
//! stdout: True
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public record Handle(Conn: Connection?);

public class Program
  public static method None(): Handle => new Handle(null);

  public static method Main()
    var h := None();
    Console.WriteLine(h.Conn = null);
  end Main;
end Program;
