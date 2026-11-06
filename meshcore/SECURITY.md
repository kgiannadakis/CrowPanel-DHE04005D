# Security

Follow the repository [security policy](../SECURITY.md) to report vulnerabilities
privately. Do not include real device identities, credentials or unredacted logs.

The P4 dashboard is HTTP-only and unauthenticated. Remembered repeater passwords
are plaintext local storage, and translation sends text to an external service
without a fully verified TLS path. These limitations are documented in the
[development guide](docs/crowpanel.md#feature-limits-and-security).

No older-version support or response-time guarantee is made for this community
fork. Report issues affecting an upstream component to its maintainers as well.
