# Endpoint Profiles + Chat-Footer Switcher (S6.16)

Multi-source LLM support: a global list of OpenAI-compatible endpoints at
`~/.locus/endpoints.json`, editable in Settings > Endpoints, switchable
per-tab from the chat footer without reopening the workspace.

The six seed profiles are LM Studio (local) / Ollama (local) / NVIDIA Build /
OpenAI / OpenRouter / Claude-via-proxy. Builtins can be edited but not removed.

## Test 1 -- Settings > Endpoints round trip + secret handling

1. Open Settings (Ctrl+,) and select the **Endpoints** tab (second tab,
   after LLM). The list shows six builtin rows: Name | URL | Model | Key, with
   the active row prefixed `*`. The Key column reads `(none)` for the local
   profiles and a masked `****abcd` once a key is set.
2. Select **NVIDIA Build (hosted)** and click **Edit...**. The Name field is
   greyed (the name is the store key). Paste an API key into the API key
   field -- it shows as dots. Tick **Show key**: the full key becomes visible
   via the field's tooltip (native password fields can't un-mask in place).
   Click OK.
   - Expected: the NVIDIA row's Key column now shows `****` + the last 4 chars.
3. Click **Add...**, give it a Name + Base URL, leave the rest default, OK.
   - Expected: a new non-builtin row appears. Select it and confirm **Remove**
     is enabled (greyed for builtin rows).
4. Select the NVIDIA row, click **Set Active** (the `*` marker moves to it),
   then OK the Settings dialog.
5. Reopen Settings > Endpoints.
   - Expected: the key, the added row, and the active marker all survived.
     Confirm `~/.locus/endpoints.json` on disk contains the profiles and your
     key in plaintext (this is by design -- OS file ACLs are the boundary).

**Hard fail:** the API key appears anywhere in `.locus/locus.log` at any log
level. Search the log for the key string -- it must not be present.

## Test 2 -- Chat-footer picker + mid-session hot-swap

Needs two reachable endpoints. Easiest: LM Studio at `127.0.0.1:1234` plus a
second LM Studio / Ollama instance, or a real NVIDIA key on the NVIDIA profile.

1. In the chat footer (the mode-switcher row, left of the permission dropdown)
   find the **endpoint picker**. Its tooltip shows the active profile's base
   URL + masked key. Send a message and confirm it answers on the current
   endpoint.
2. Without closing the workspace, pick a different profile from the picker.
   - Expected: a status-bar line `Endpoint: <name> (<model>)` appears once the
     swap completes. The chat history is preserved (no reset). `.locus/locus.log`
     shows `AgentCore: endpoint switched to '<name>' (...)`.
3. Send a second message.
   - Expected: it is served by the new endpoint (different model voice / speed;
     the log's `LLM client: endpoint=<new base_url>` confirms it).
4. While a turn is still streaming, switch the picker again.
   - Expected: the log shows `endpoint switch ... deferred until turn complete`;
     the swap applies before the *next* message, not mid-stream. History
     intact.
5. Pick the trailing **Edit endpoints...** row.
   - Expected: Settings opens directly on the Endpoints tab; the picker
     selection snaps back to the previously active profile (the sentinel is
     not itself a selectable endpoint).

## Test 3 -- Per-workspace override + legacy compatibility

1. With more than one workspace available, set the active endpoint to NVIDIA in
   workspace A (Settings > Endpoints > Set Active, OK). Open workspace B in a
   second Locus window.
   - Expected: workspace B opens on whatever its own `llm.active_endpoint`
     resolves to. A workspace with no override follows the global store's
     active; one pinned to a different profile keeps its pin. The two windows
     can run different endpoints side by side.
2. Legacy check: in a workspace whose `.locus/config.json` has a non-default
   `llm.endpoint` (e.g. a LAN box) and no `llm.active_endpoint`, open Locus.
   - Expected: the agent still talks to that legacy endpoint with no Settings
     interaction required (the resolved config keeps the legacy URL). On the
     first run after upgrade, a `Migrated (<workspace>)` profile appears in
     Settings > Endpoints carrying that URL, and the workspace is pinned to it.
