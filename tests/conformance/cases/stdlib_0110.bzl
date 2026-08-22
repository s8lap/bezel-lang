record ValueChangedEvent(Sender: Slider, OldValue: decimal, NewValue: decimal);
record DataReceivedEvent(Sender: Connection, Payload: Array<byte>, Timestamp: DateTime);
record ProcessResult(Success: bool, Message: string);
