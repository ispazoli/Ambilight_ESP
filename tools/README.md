# Tools

This directory is reserved for development and validation utilities.

Recommended tooling areas:

- firmware/browser parity inventory;
- endpoint inventory;
- JSON schema checks;
- Mood ID parity checks;
- 3×10 smoke tests;
- regression reports;
- repository secret scans.

Tools should be deterministic and should fail loudly when an expected contract is missing.

Do not place real credentials in tool configuration or test fixtures.
