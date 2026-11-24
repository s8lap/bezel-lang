# n04-contracts

*Area:* contracts

An `Account` class with `requires(amount > 0m)` on `Deposit`/`Withdraw`, `requires(amount <= Balance)` on `Withdraw`, an `ensures` on the balance after `Deposit`, and an `invariant(_balance >= 0m)`. Built with `--checked-contracts`. Deposit 100, withdraw 30, print `balance 70`; then attempt `Withdraw(500m)`, which is a caller bug; `Main` (the boundary) catches `ContractViolationException` and prints `bug: contract violated` and the balance unchanged `balance 70`. Also a `pure` helper `Fee(amount)` (1% rounded to 2 places) used in an `ensures`: print `fee 0.30` for 30.
