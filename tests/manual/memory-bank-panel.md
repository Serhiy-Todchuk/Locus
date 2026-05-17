# Manual Test Plan -- Memory Bank Panel (S5.K)

The dockable Memory Bank panel: AUI-docked on the right (default hidden),
toggled via **View > Memory Bank** (Ctrl+M). Lists every live entry from
`.locus/memory/`, with search / filter / bulk-ops / edit-in-place / soft-delete
/ restore. Pure view over [`MemoryStore`](../../src/core/memory_store.h);
mutations dispatch back through the store.

The legacy `/memorize` + `/forget` slash commands and the `.locus/memory/*.md`
on-disk layout are unchanged -- see [memory-bank.md](memory-bank.md) for the
data-layer plan. This plan only exercises the new UI.

## Test 1 -- Round-trip: add, filter, edit, soft-delete, restore

1. Open the workspace. From the chat input run:
   `/memorize +build cmake is the build tool`
   `/memorize --pin always reply in Cyrillic`
   `/memorize +editor +unrelated agent-added note`
2. **View > Memory Bank** (or Ctrl+M). The right-side dock opens with three
   rows. The pinned entry is first; the star column on its row shows a pin
   glyph. **Expected:** three rows visible, pinned first.
3. Type `cmake` into the **Search** box. **Expected:** only the cmake row
   remains; the refresh is debounced ~200 ms so it doesn't flicker per
   keystroke.
4. Clear the search box. Set the **Source** dropdown to `user`.
   **Expected:** all three rows present (slash-added entries are tagged
   `source=user`). Change to `agent` -- list empties. Reset to `all`.
5. Set the **Tag** dropdown to `build`. **Expected:** only the cmake row.
   Reset to `(any)`.
6. Click a row. The detail pane on the bottom populates with full content,
   tags (comma-separated), and a `Pinned` checkbox. Edit the content
   ("cmake is the build tool, not make"), append a tag (`, tooling`), and
   click **Save**. **Expected:** the list row updates; the entry's
   `updated_at` in the on-disk `.locus/memory/<id>.md` bumps; the tags
   chip in the list row reads `build, tooling`.
7. Select the same row, click **Delete**, confirm. **Expected:** the row
   disappears; `.locus/memory/<id>.md` is moved under
   `.locus/memory/.deleted/<id>.md`.
8. Tick **Show deleted**. **Expected:** the bulk-op buttons (Pin/Tag/Save/
   etc) grey out; a Restore button appears; the deleted row is visible
   with its content preview. Select it and click **Restore**. **Expected:**
   the row returns to the live list when you untick **Show deleted**.

**Hard fail conditions:** an entry survives in the live list after Delete
without the `.deleted/` move (would mean the soft-delete pathway broke);
edits to the detail pane don't persist after a panel close + reopen
(would mean Save dispatched but didn't write the file); pinned entries lose
their pin glyph after restart (the on-disk frontmatter should keep
`pinned: true`).

## Test 2 -- Bulk ops + live updates

1. Add 4-5 entries via `/memorize`.
2. Ctrl-click 3 rows in the panel. Click **Pin/Unpin**. **Expected:** all
   three flip to pinned (pin glyph appears for each). Click again -> all
   three unpin.
3. With 2 rows selected, click **Add tags...**, enter `important, review`.
   **Expected:** both rows now show `important, review` in their tags
   column; existing tags are preserved (set union, not replace).
4. While the panel is open and visible, in the chat input run
   `/memorize a brand new fact`. **Expected:** the new entry appears in
   the list within ~250 ms without a manual refresh -- the panel listens
   to the `memory_added` activity event and re-renders.
5. Run `/forget <id-from-step-1>` for one of the older entries.
   **Expected:** that row disappears from the live list within ~250 ms.

**Hard fail conditions:** the new entry from step 4 stays missing until
the panel is closed + reopened (would mean the activity-event hook never
fired); bulk pin leaves some rows pinned and some unpinned (would mean
the bulk loop bailed mid-iteration).

## Test 3 -- Capability gating

1. Open **Settings > Capabilities** and uncheck **Memory Bank**. OK.
2. Re-open the workspace (the capability flag governs whether the
   `MemoryStore` is created at open). **Expected:** **View > Memory Bank**
   is greyed out in the menu; if the AUI perspective had the pane open,
   it's force-closed; clicking elsewhere doesn't pop the panel back.
3. Re-enable **Memory Bank** in Settings; reopen the workspace.
   **Expected:** the menu item is enabled again; opening the panel
   restores all prior entries (the `.locus/memory/` directory survived
   the capability toggle).

**Hard fail conditions:** the menu item stays clickable when the
capability is off (would mean `update_memory_bank_menu_state` didn't run);
toggling the capability back on with existing entries shows an empty list
(would mean the store didn't reload).
