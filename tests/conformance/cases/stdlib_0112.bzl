// The payload here is a `string`-only record, so it is not borrow-bearing (spec §9.7)
// and is admissible as an `async` lambda's parameter (spec §30.1):
//   record MessageReceivedEvent(Channel: string, Body: string);
_messageReceived += async lambda(e) => do
  try
    await ProcessMessageAsync(e.Body);
  catch
    var ex: Exception => Log($"Handler failed: {ex.Message}")
  end;
end;
