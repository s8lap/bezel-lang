# n17-observer

*Area:* borrows

A `Thermostat` exposing a temperature-changed event (an `owned readonly EventSource<double>` field exposed as `IEvent<double>`), and two observers: a `Display` that keeps a back-reference to the thermostat (given at construction) to print `display: 21.5 (target 20)` and an `Alarm` that prints `alarm: too hot 26` above 25. Set temperatures 21.5, 26.0; then dispose the alarm's subscription and set 27.0 (only the display prints). Record the trust-report lines.
