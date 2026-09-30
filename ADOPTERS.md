# Adopters — Built with UE Protocol

Projects, radios, and apps that implement or embed **UltraEdge Protocol**. This gallery is the
ecosystem's front door and the project's main growth signal — if you build something on UE Protocol,
open a PR adding it here (see [CONTRIBUTING](CONTRIBUTING.md)).

Add a row with: project name + link, what it is, which side of the link it implements, and the
language/implementation.

| Project | What it is | Role | Implementation |
|---|---|---|---|
| [EdgeTX-UE fork](https://github.com/EdgeTX/edgetx) | EdgeTX firmware with the flag-gated companion overlay | Radio (firmware) | This repo's C++ codec, embedded in-tree |
| UltraEdge app | Android companion app — the radio's screen on your phone | App (host) | Independent Kotlin implementation of the same wire |

> The two seed entries are UE Protocol's own reference implementations (one firmware, one app),
> intentionally in two different languages to prove the spec — not the codec — is the contract.
> Links resolve once each project is published (see the private repo's release plan).
