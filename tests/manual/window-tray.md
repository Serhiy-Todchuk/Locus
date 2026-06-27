# Window minimize + Hide to Tray

Covers the split between plain minimize (normal taskbar behaviour) and the
explicit Hide to Tray action. Minimize must never make the window vanish from
the taskbar; only Hide to Tray (View menu / Ctrl+Shift+H) sends the window into
the notification area.

## Test 1 -- Minimize stays a normal window

1. Launch Locus on any workspace.
2. Click the window's minimize button (or press the OS minimize shortcut).
   - Expected: the window minimizes to the taskbar like any other app. Its
     taskbar button is still present. The window did NOT disappear into the
     notification area.
3. Click the taskbar button (or Alt+Tab to Locus).
   - Expected: the window restores normally.
4. Minimize again, then double-click the Locus tray icon near the clock.
   - Expected: the window restores to a normal (non-minimized) window, not an
     invisible minimized one.

Hard fail: if step 2 removes the taskbar button or hides the window entirely,
the minimize/Hide-to-Tray split is broken.

## Test 2 -- Hide to Tray round trip

1. With the window visible, open View > Hide to Tray (or press Ctrl+Shift+H).
   - Expected: the window disappears from the screen AND from the taskbar. The
     tray icon near the clock remains visible.
2. Double-click the tray icon.
   - Expected: the window comes back as a normal restored window and is raised
     to the front.
3. Hide to Tray again, then right-click the tray icon and choose "Show Window".
   - Expected: same as step 2 -- window restored and raised.
4. With the window visible, right-click the tray icon and choose "Hide Window".
   - Expected: the window hides into the tray (same as Hide to Tray). Choosing
     the same menu entry again (now labelled "Show Window") brings it back.

## Test 3 -- Minimize, then Hide to Tray, then restore

1. Minimize the window to the taskbar.
2. Press Ctrl+Shift+H (the accelerator still works while minimized via the
   menu/global accelerator) OR restore first and then Hide to Tray.
   - Expected: the window ends up hidden into the tray with no taskbar button.
3. Double-click the tray icon.
   - Expected: the window returns as a normal, non-minimized, raised window --
     never an invisible minimized one.
