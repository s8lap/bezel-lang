public record Port
  public readonly property Value: int;

  public constructor(value: int)
    requires(value >= 0 and value <= 65535)

    Value := value;
  end constructor;
end Port;

var p := default(Port);            // ERROR: not defaultable
var q: Port;                       // ERROR: default-initialized storage
var slots: [8]Port;                // ERROR: default slots
var r := p with(Value := -1);      // ERROR: `with` is rejected on a constructor-only record
