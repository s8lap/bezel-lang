Clicked.Raise(new ClickedEvent(this, x, y, button));                    // synchronous
await DataReceived.RaiseAsync(new DataReceivedEvent(this, payload, t)); // asynchronous
ApplicationExiting.Raise();                                              // no-payload
