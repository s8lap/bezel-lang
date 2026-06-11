public record User(
    Id: int,
    FirstName: string        serialized "first_name",
    InternalToken: string    serialized skip,
    LastSeen: DateTime?      serialized "last_seen" optional
): ISerializable, IDeserializable;
// synthesized form codes {"Id": …, "first_name": …, "last_seen": …};
// InternalToken is excluded; last_seen may be absent on read
