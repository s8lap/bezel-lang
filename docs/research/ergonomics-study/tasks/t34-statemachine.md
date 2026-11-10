# t34-statemachine

*Area:* state machine

A vending machine with states Idle, HasCoin(amount), Vending. Events: Coin(n), Select(price), Cancel. Rules: Coin adds to credit; Select vends if credit >= price, returning change; Cancel refunds. Process: Coin 50, Select 75 (rejected: `insufficient: 50 < 75`), Coin 50, Select 75 (`vend, change 25`), Cancel in Idle (`nothing to refund`). Print each transition as `Idle -> HasCoin(50)` style lines.
