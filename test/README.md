# Tests

The repository has two lightweight checks that run without an ESP8266:

- `run_dashboard_tests.sh` compiles and runs the host-side PVE model tests.
- `verify_pve_dashboard.sh` checks source-level integration invariants, including
  active PVE/NAS calls, required API endpoints, bounded VM parsing, certificate
  pinning, canonical node handling, and local secret isolation.

Run both from the repository root:

```sh
sh test/run_dashboard_tests.sh
sh test/verify_pve_dashboard.sh
```

The source verifier is a commit gate, not a substitute for compiling the
firmware or validating API responses and heap behavior on the board.
