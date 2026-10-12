//! case: s3_1_0003
//! doc: spec
//! section: 3.1
//! title: the unit type has exactly one value
//! rule: The unit type `()` — exactly one value, also written `()`.
//! expect: run
//! stdout: ()
module UnitTest;

public class UnitTest
  public static method Main()
    var u: () := ();
    Console.WriteLine(u);
  end Main;
end UnitTest;
