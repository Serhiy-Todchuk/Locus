# Locus -- Quickstart

Five steps, ~10 minutes, then you're chatting with a local model about your own
code or documents. Windows 11 + an OpenAI-compatible local LLM server (LM Studio
in this guide).

If anything below fails, jump to [Troubleshooting](#troubleshooting). For deeper
setup, build-from-source, or CLI use, see the full [README.md](README.md).

---

## 1. Install LM Studio and load a model

Download and install [LM Studio](https://lmstudio.ai/).

In LM Studio: search for **Gemma 4 E4B**, click Download (~3 GB). When it
finishes, load the model and click **Start Server** (default port `1234`).
Leave LM Studio running.

Any tool-calling model works -- Gemma 4 E4B @ 8k context is the minimum
verified target. Stronger picks if you have the VRAM: **Qwen 3 14B / 32B**
or **GPT-OSS 20B**. Models without tool-calling training will fail most tasks.

## 2. Get Locus

Download the latest `locus-windows-x64.zip` from the
[Releases page](https://github.com/Serhiy-Todchuk/locus/releases) and unzip it
anywhere -- e.g. `C:\Tools\locus\`. You should see:

```
locus\
  locus_gui.exe
  locus.exe
  pdfium.dll
  models\
  download.ps1
  download-small.ps1
```

Building from source instead? See [README.md](README.md#first-time-setup-on-a-new-pc).

## 3. Download embedding + reranker models

From the unzipped folder:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\download.ps1
```

Pulls `bge-m3-Q8_0.gguf` (multilingual embedder) and `bge-reranker-v2-m3-Q8_0.gguf`
(cross-encoder) into `models\`. Total ~1.27 GB, no Hugging Face account needed.

Tight on disk or English-only corpus? Use `.\download-small.ps1` instead
(~58 MB total, faster, weaker on long passages).

The reranker is **off by default** for performance reasons. Turn it on in
Settings -> Index once everything else works, if you want better top-K
precision on semantic search.

## 4. Launch Locus

```
locus_gui.exe D:\path\to\my\project
```

Or run `locus_gui.exe` with no arguments to pick a folder from a dialog.

What happens on first open:

- Locus creates `.locus\` inside your workspace (config, index, sessions, logs).
- It indexes the folder (seconds to a minute, depending on size).
- It connects to LM Studio and auto-detects the loaded model + context length.
- Embeddings populate in the background after the structural index is built.

Add `.locus/` to your `.gitignore` -- everything in there is local state.

## 5. Use it

Type a message in the chat panel. The agent will propose tool calls (read
files, search, run commands, edit files). Each one appears in the approval
panel at the bottom -- click **Approve**, **Modify**, or **Reject**.

Useful from the start:

| What | How |
|---|---|
| Multi-step task with a checkpoint | Toggle **Plan** mode above the input. Agent proposes a plan, you approve it, then it executes step by step. |
| Revert an agent's last edits | Click **Undo** in the chat footer, or type `/undo`. |
| Free up context space | Type `/compact` or click the context meter when it warns. |
| Save / resume a conversation | `/save` then `/sessions` then `/load <id>`. |
| Inspect what happened | Activity panel on the right (Log + Metrics tabs). |
| Run diagnostics | Launch with `-verbose` and read `<workspace>\.locus\locus.log`. |

Full slash-command list and flags: [README.md](README.md#cli-commands).

---

## Troubleshooting

**Cannot connect to LLM server.** LM Studio isn't running, or the server
wasn't started. Open LM Studio -> load a model -> *Start Server*. The
endpoint is `http://127.0.0.1:1234`.

**LLM stream stalled after retry.** A large model on first prompt can take
minutes just to prefill. Raise the watchdog in `<workspace>\.locus\config.json`:

```json
"llm": {
  "timeout_ms": 1200000
}
```

**Semantic search disabled / model file not found.** You skipped step 3, or
the `models\` folder isn't next to the exe. The exe walks up to 6 parent dirs
looking for `models\`, so the dev layout (exe deep in `build\`, models at repo
root) works without copying.

**Workspace locked.** Another Locus already has that folder open, or a
previous run crashed before releasing the lock. Delete
`<workspace>\.locus\locus.lock` and retry.

**Anything else weird.** Relaunch with `-verbose` and read
`<workspace>\.locus\locus.log` -- every SQL query, tool call, and LLM token
ends up there. If you're filing a bug, attach the relevant log lines.
