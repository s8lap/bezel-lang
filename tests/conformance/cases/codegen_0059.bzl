public record Tally
  public property Hits: int;
  public method Bump()
    Hits := Hits + 1;              // ERROR: `this` is a BORROW of the receiver value
  end Bump;
end Tally;

public record Session(owned Log: FileStream);      // ERROR: records cannot carry disposal obligations
