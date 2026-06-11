connection.DataReceived += async (e) => do
  try
    await ProcessPayloadAsync(e.Payload);
  catch
    var ex: Exception => Log($"Handler failed: {ex.Message}")
  end;
end;
