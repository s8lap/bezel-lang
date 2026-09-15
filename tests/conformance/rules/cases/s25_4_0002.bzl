//! case: s25_4_0002
//! doc: spec
//! section: 25.4
//! title: a named factory method is the sanctioned conversion between user types
//! rule: To express a conversion between user types, define a named factory or
//!   instance method.
//! cites: 25.4
//! expect: run
//! stdout: 7
module Conf;

public record Dto(Id: int)
  public method ToUser(): User => new User(Id);
end Dto;

public record User(Id: int);

public class Program
  public static method Main()
    var d := new Dto(7);
    var u := d.ToUser();
    Console.WriteLine(u.Id);
  end Main;
end Program;
