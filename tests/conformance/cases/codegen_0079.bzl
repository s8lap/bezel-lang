public record Port
  public readonly property Value: int;

  public constructor(value: int)
    requires(value >= 0 and value <= 65535)

    Value := value;
  end constructor;
end Port;
