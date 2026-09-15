//! case: s18_7_0001
//! doc: spec
//! section: 18.7
//! title: an interpolated verbatim string combines suppressed escapes with interpolation holes
//! rule: "`$@` combines both" — escape suppression and interpolation. Example:
//!   `var documentPath := $@"C:\Users\{user}\Documents\report.pdf";` renders the
//!   backslashes literally and substitutes the hole.
//! cites: 18.7, 18.5, 18.6
//! expect: run
//! stdout: C:\Users\bob\Documents\report.pdf
module Conf;

public class Program
  public static method Main()
    var user := "bob";
    var documentPath := $@"C:\Users\{user}\Documents\report.pdf";
    Console.WriteLine(documentPath);
  end Main;
end Program;
