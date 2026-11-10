# t10-validate

*Area:* data modelling

Validate signup records (email, age, password). Rules: email must contain `@` with text before and a `.` after it; age in 13..120; password at least 8 chars and contain a digit. Collect ALL errors per record. For three records print `ok: ann@example.com` or `invalid: bob: email missing @; age out of range` style lines (errors in rule order, joined with `; `).
