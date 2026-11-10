// Probe p11: `throw x` consumes x; an exception may own a payload; the runtime
// disposes the exception (and its owned payload) after the matching arm
// completes (spec §31.1 exception ownership, K3.13 site-announced kill).
module P11;

public class Detail
  private _name: string;
  public constructor(name: string)
    _name := name;
  end constructor;
  public property Name: string => _name;
  destructor
    Console.WriteLine($"  dispose Detail {_name}");
  end destructor;
end Detail;

public class ValidationException: Exception
  private owned _info: Detail;
  public constructor(msg: string, owned info: Detail) base(msg);
    _info := info;
  end constructor;
  public property Info: Detail => _info;
end ValidationException;

public class Program
  public static method Validate(s: string)
    if s.Length = 0 then
      var ex := new ValidationException("empty", new Detail("field=name"));
      throw ex;                      // ex is consumed here: the keyword announces the kill
    end if;
  end Validate;

  public static method Main()
    try
      Program.Validate("");
    catch
      var ex: ValidationException => do
        Console.WriteLine($"  in arm: {ex.Message} / {ex.Info.Name}");
      end,
    end try;
    Console.WriteLine("  after try");
  end Main;
end Program;
