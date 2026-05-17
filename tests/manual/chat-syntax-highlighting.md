## Manual Test Plan -- Chat Syntax Highlighting

**Feature:** Code blocks in assistant messages render with syntax colors
(keywords, types, strings, comments) for ~28 languages, powered by a Prism
bundle shipped under `resources/prism/` next to `locus_gui.exe`. No CDN
fetch -- highlighting must work offline.

---

### Test 1 -- Highlighting works, including offline

1. Launch `locus_gui.exe` and open any workspace.
2. Ask the agent: *"Show me a short C++ example: a class with a constructor,
   a templated method using `std::vector` and `std::string`, a try/catch
   block, and a comment. Just the code, no explanation."*
3. Wait for the reply.
4. Disconnect from the network (turn off Wi-Fi or unplug Ethernet).
5. Ask: *"Now do the same in Python."* and then *"Now in Rust."*

**Expected**

- Each fenced code block in the chat renders with distinct colors:
  keywords (`class`, `template`, `try`, `def`, `fn`) in one color, types
  and identifiers in another, string literals in a third, comments dim
  or italic.
- Steps 4-5 succeed identically -- highlighting stays on with no network.
- Hard fail: if step 5 renders any code block in plain monochrome
  monospace, the bundle either didn't ship next to the exe or the chat
  panel is falling back to network resources somewhere.

---

### Test 2 -- Missing bundle degrades gracefully

1. Quit `locus_gui.exe`.
2. Rename `resources/prism/` (next to `locus_gui.exe`) to
   `resources/prism.bak/`.
3. Launch `locus_gui.exe` again, open a workspace, and ask for any code
   snippet.

**Expected**

- The chat panel loads normally and the reply streams in.
- The code block renders in plain monospace (no colors) but is fully
  readable -- newlines, indentation, fenced code wrapping all intact.
- `.locus/locus.log` contains a `ChatPanel: Prism asset missing:` warning
  naming each of the three expected files.
- Hard fail: if the chat panel is blank, hangs, or shows a JS error
  overlay, the asset-read helper is throwing instead of returning empty.

Restore the directory (rename `resources/prism.bak/` back to
`resources/prism/`) before the next test session.
