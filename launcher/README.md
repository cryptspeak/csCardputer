# Ratspeak Launcher

This is the local mode chooser for Cardputer Adv. It does not download or
install firmware. It only selects between the Standalone and RNode app partitions
already flashed into internal storage.

Current controls:

- `Enter`: start selected mode.
- `;`, `,`, `W`: select Standalone.
- `.`, `/`, `S`: select RNode.
- `R`: start Standalone.
- `N`: start RNode.

If no key is pressed, the launcher starts the last-used mode after seven
seconds. Any keyboard input cancels auto-start for that boot.
