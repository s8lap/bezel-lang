//! case: s27_7_1_0007
//! doc: spec
//! section: 27.7.1
//! title: two type-qualified references to one extension method are equal and differ from another method's
//! rule: A reference that captures no receiver — a static method reference to
//!   `M`, or the extension-method reference to `M` that §27.6 forms
//!   type-qualified, the receiver being the function's first parameter —
//!   equals every other such reference to `M`
//! cites: 27.7.1, 27.6
//! expect: run
//! stdout: True False
module Conf;

public record Tag(Name: string);

extension Tag
  public method Shout(): string => this.Name + "!";
  public method Whisper(): string => this.Name;
end;

public class Program
  public static method Main()
    var f: (Tag) -> string := Tag.Shout;
    var g: (Tag) -> string := Tag.Shout;
    var h: (Tag) -> string := Tag.Whisper;
    Console.WriteLine($"{f = g} {f = h}");
  end Main;
end Program;
