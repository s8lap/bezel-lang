//! case: s3_4_0007
//! doc: spec
//! section: 3.4
//! title: a user type may shadow a library collection type and keeps its own members
//! rule: Language spec §3.4 (v0.345): "This does not reserve other standard-library type
//!   names — only the PascalCase aliases of the primitive value types and String; an ordinary
//!   library type such as `Array` or `Stream` may still be shadowed by a user type." A user
//!   `Array` therefore names the user's own type, and that type's own member is the one a call
//!   on it reaches.
//! cites: 3.4
//! expect: run
//! stdout: USER-REV
module Conf;

class Array
  public method Reversed(): Array
    Console.WriteLine("USER-REV");
    return this;
  end Reversed;
end Array;

public class Program
  public static method Main()
    var a: Array := new Array();
    _ := a.Reversed();
    Console.WriteLine("after");
  end Main;
end Program;
