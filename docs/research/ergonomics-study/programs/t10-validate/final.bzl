module Model.Validate;

public record Signup(Email: string, Age: int, Password: string);

public class Validator
  public static method Validate(s: Signup): owned Array<string>
    var errors := new Array<string>();
    var at := s.Email.IndexOf('@');
    if at < 0 then
      errors.Add("email missing @");
    elif at = 0 or s.Email.IndexOf('.', at) < 0 then
      errors.Add("email malformed");
    end;
    if not (s.Age in 13..120) then
      errors.Add("age out of range");
    end;
    if s.Password.Length < 8 then
      errors.Add("password too short");
    end;
    if not s.Password.Any(lambda(c) => Char.IsDigit(c)) then
      errors.Add("password needs a digit");
    end;
    return errors;
  end Validate;
end Validator;

public class Program
  public static method Main()
    var signups: Array<Signup> := [
      new Signup("ann@example.com", 30, "hunter22x"),
      new Signup("bob", 7, "secret12"),
      new Signup("@x", 40, "short"),
    ];
    for var s in signups do
      var errs := Validator.Validate(s);
      if errs.Count = 0 then
        Console.WriteLine($"ok: {s.Email}");
      else
        Console.WriteLine($"invalid: {s.Email}: {String.Join("; ", errs)}");
      end;
    end;
  end Main;
end Program;
