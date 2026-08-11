public class Greeter(name: string, prefix: string)
  // `prefix` flows into _label's initializer and is never used elsewhere — not captured.
  private _label: string := prefix + ": ";
  
  // `name` is referenced in Greet's body — captured as a private field.
  public method Greet(): string => _label + name;
end Greeter;
