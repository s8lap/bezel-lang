//! case: s3_3_0003
//! doc: spec
//! section: 3.3
//! title: a nullable value type is an optional with a value or null
//! rule: Value types may also be wrapped in ? to produce an optional.
//! expect: run
//! stdout: has value 42
//! cites: 3.3
module OptionalVal;

public class OptionalVal
  public static method Main()
    var x: int? := 42;
    if x is not null then
      Console.WriteLine($"has value {x}");
    end;
  end Main;
end OptionalVal;
