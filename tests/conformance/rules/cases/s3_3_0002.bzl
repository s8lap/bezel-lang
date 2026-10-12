//! case: s3_3_0002
//! doc: spec
//! section: 3.3
//! title: a nullable reference type variable may hold null
//! rule: A reference type is non-nullable by default. A trailing `?`
//!   produces its nullable form.
//! expect: run
//! stdout: null
module NullableRef;

public class NullableRef
  public static method Main()
    var x: string? := null;
    Console.WriteLine(x);
  end Main;
end NullableRef;
