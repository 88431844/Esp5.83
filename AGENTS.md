# Project workflow

- After every requested firmware or dashboard change, run the relevant tests and compile the production firmware.
- If verification succeeds, flash the firmware to the connected device before reporting the work complete.
- Do not push changes, open a pull request, or otherwise submit changes to GitHub unless the user explicitly decides to do so.
- Clearly report whether flashing succeeded and include the detected device port.
