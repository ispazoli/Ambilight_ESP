# Security Policy

## Scope

This project controls network-connected ESP32 hardware and exposes HTTP/WebSocket functionality on the local network.

## Do not commit secrets

Never commit:

- Wi-Fi passwords
- web authentication passwords
- API keys
- access tokens
- private keys or certificates
- personal cloud credentials
- production firmware binaries containing embedded credentials

Use local ignored files such as `secrets.h` for development credentials.

## Public repository rule

Assume every committed value is public and permanent.

Removing a secret from the current file is not sufficient if it was previously committed. If a real credential was exposed:

1. revoke or rotate it immediately;
2. remove it from the current source;
3. inspect repository history;
4. rewrite history if necessary;
5. force-push only after understanding the consequences for collaborators.

## Network security

The controller is intended for a trusted local network unless additional network isolation and authentication are implemented.

Do not expose the ESP32 HTTP/WebSocket ports directly to the public Internet.

Use strong unique credentials when web authentication is enabled.

## Reporting a vulnerability

For a security-sensitive issue, avoid publishing active credentials or exploit details in a public issue. Contact the project maintainer privately through the GitHub account associated with this repository.

## Credential audit

Before a public release, scan the repository and its history for terms such as:

```text
password
api_key
apikey
token
secret
Authorization
Bearer
BEGIN PRIVATE KEY
WIFI
SECRET_
```

Also search for known historical credential strings if the project has previously contained them.

## Release requirement

A release must not be marked secure merely because the current working tree is clean. Repository history must also be considered.
