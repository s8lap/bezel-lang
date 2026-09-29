public record Port
  public readonly property Value: int;

  public constructor(value: int)
    requires(value >= 0 and value <= 65535)

    Value := value;
  end constructor;
end Port;

var p := default(Port);            // ERROR: not defaultable
var q: Port;                       // fine: a local is uninitialized, never default-filled (§7.9)
_ := q.Value;                      // ERROR: `q` read before it is assigned
var slots: [8]Port;
slots[0] := new Port(1);           // ERROR: an element store uses the unassigned array; assign the whole array
var r := p with(Value := -1);      // ERROR: `with` is rejected on a constructor-only record
