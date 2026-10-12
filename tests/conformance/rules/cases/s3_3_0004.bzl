//! case: s3_3_0004
//! doc: spec
//! section: 3.3
//! title: a null-tested nullable reference is narrowed in the true branch
//! rule: A reference type is non-nullable by default. A trailing `?`
//!   produces its nullable form.
//! expect: run
//! stdout: hello
//! cites: 3.3, 7.4
module NullableFlow;

public class NullableFlow
  public static method Main()
    var s: string? := "hello";
    if s <> null then
      Console.WriteLine(s);
    end;
  end Main;
end NullableFlow;
