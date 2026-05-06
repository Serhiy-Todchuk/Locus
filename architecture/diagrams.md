# Locus -- Architecture Diagrams

Per-subsystem visual map of what runs under the hood. All diagrams are
Mermaid -- GitHub renders them natively, no images checked in.

For prose-level depth see the sibling docs in this folder
([overview](overview.md), [agent-loop](agent-loop.md),
[threading-model](threading-model.md), [tool-protocol](tool-protocol.md),
[workspace-index](workspace-index.md)). This file is the visual index, not
the source of truth.

---

## 1. System Overview

Top-level boxes -- one `LocusSession` per opened folder, any number of
attached frontends, a single LLM endpoint per session.

```mermaid
flowchart TB
    User([User])
    User --> Frontends

    subgraph Frontends["Frontends (1+ attached)"]
        direction LR
        CLI[CliFrontend<br/>terminal]
        GUI[wxWidgets GUI<br/>via WxFrontend bridge]
        Future[Future:<br/>Crow / VSCode shim]
    end

    Frontends <-->|FrontendRegistry<br/>thread-safe fan-out| Session

    subgraph Session["LocusSession (per workspace)"]
        direction TB
        Agent[AgentCore<br/>orchestrator + agent thread]
        LLM[ILLMClient]
        Tools[ToolRegistry<br/>13 built-ins]
        WS[Workspace<br/>: IWorkspaceServices]
    end

    Agent -->|stream tokens<br/>+ tool calls| LLM
    Agent -->|dispatch| Tools
    Tools -->|read / write| WS
    LLM -->|HTTP + SSE| LMS[(LM Studio<br/>localhost:1234)]

    subgraph WSI["Workspace internals"]
        direction LR
        Index[Indexer + IndexQuery]
        Watcher[FileWatcher + Pump]
        Ext[ExtractorRegistry<br/>md/html/pdf/docx/xlsx/zim]
        Procs[ProcessRegistry<br/>Win32 Job Object]
        Emb[Embedder<br/>bge-m3]
        Rer[Reranker<br/>bge-reranker-v2-m3]
    end

    WS --- WSI

    Index --> Main[(".locus/index.db<br/>files + fts5 +<br/>symbols + headings")]
    Index --> Vec[(".locus/vectors.db<br/>chunks + chunk_vectors<br/>via sqlite-vec")]

    Watcher -.->|debounced events| Index
    Emb -.->|1024-dim vectors| Vec
```

---

## 2. Agent Loop -- One User Turn

End-to-end flow from "user hits Send" to "final assistant message". Tool
calls re-enter the LLM step until the model emits a terminal response.

```mermaid
flowchart TB
    Start([User message]) --> Diff{File changes<br/>since last turn?}
    Diff -->|yes| Prep["Prepend<br/>'[Files changed: a, b, c (deleted)]'<br/>to user msg"]
    Diff -->|no| Push
    Prep --> Push[Append to<br/>ConversationHistory]

    Push --> Arm[ToolDispatcher.set_turn_context<br/>arms CheckpointStore for turn]
    Arm --> Clear[FileChangeTracker.<br/>clear_agent_touched]
    Clear --> Step

    subgraph Loop["AgentLoop.step (repeat until terminal)"]
        direction TB
        Step[Build messages<br/>+ tool schema]
        Step --> Stream[OpenAiTransport<br/>POST /chat/completions<br/>SSE stream]
        Stream --> Dec[IStreamDecoder<br/>Auto / OpenAI / Qwen / Claude / None]
        Dec --> SinkE{event}
        SinkE -->|on_text| FE[Stream to frontends]
        SinkE -->|on_reasoning| FE
        SinkE -->|on_tool_call_delta| Acc[Accumulate ToolCall]
        SinkE -->|on_usage| Budg[ContextBudget +<br/>MetricsAggregator]
        Acc --> Result[AgentStepResult]
    end

    Result --> Has{tool_calls<br/>present?}
    Has -->|no| End([Final message<br/>+ snapshot tracker<br/>+ flush metrics])
    Has -->|yes| Disp[ToolDispatcher.execute]

    Disp --> Gate{Approval gate}
    Gate -->|reject| RJ[Inject rejection<br/>tool_result] --> Step
    Gate -->|approve / modify| Mut{mutating tool?}
    Mut -->|yes| Snap[CheckpointStore.<br/>snapshot pre-write]
    Mut -->|no| Exec
    Snap --> Exec[Tool.execute]
    Exec --> Mark[FileChangeTracker.<br/>mark_agent_touched]
    Mark --> Inj[Append tool_result<br/>to history]
    Inj --> Step
```

---

## 3. Tool Approval & Checkpointing

The gate every tool passes through. User has three choices; mutating tools
also snapshot for `/undo`.

```mermaid
sequenceDiagram
    autonumber
    participant Loop as AgentLoop
    participant Disp as ToolDispatcher
    participant Front as Frontend(s)
    participant User
    participant Ckpt as CheckpointStore
    participant Tool as ITool
    participant Hist as ConversationHistory
    participant Met as MetricsAggregator

    Loop->>Disp: dispatch(ToolCall)
    Disp->>Front: emit tool_approval_request
    Front->>User: render diff / args / Modify pane
    User-->>Front: approve | modify | reject
    Front-->>Disp: ToolDecision

    alt rejected
        Disp->>Hist: append rejection tool_result
    else approved or modified
        opt mutating: edit_file / write_file / delete_file
            Disp->>Ckpt: snapshot(rel_path)
            Note over Ckpt: .locus/checkpoints/<br/>session_id/turn_id/files/...
        end
        Disp->>Tool: execute(args, IWorkspaceServices&)
        Tool-->>Disp: ToolResult
        Disp->>Met: record(name, ok, ms, content_size)
        Disp->>Hist: append tool_result
    end
```

---

## 4. Workspace Index Pipeline

How a file on disk turns into FTS rows, symbol rows, and embedding vectors.
Two SQLite DBs, two connections, two writers -- no lock contention.

```mermaid
flowchart LR
    FS[(Workspace files<br/>on disk)] -->|efsw native| W[FileWatcher]
    W -->|debounced 1.5s<br/>hard cap 20s| P[WatcherPump]
    P --> IDX

    subgraph IDX["Indexer.process_events"]
        direction TB
        Read[Read file bytes] --> ER[ExtractorRegistry]
        ER --> ERkind{ext}
        ERkind -->|.md| MD[Markdown]
        ERkind -->|.html| HTML[HTML]
        ERkind -->|.pdf| PDF[PDFium]
        ERkind -->|.docx| DOCX[miniz + pugixml]
        ERkind -->|.xlsx| XLSX[miniz + pugixml]
        ERkind -->|.code| TS[TreeSitterRegistry<br/>9 grammars]
        TS --> Syms[SymbolExtractorRegistry<br/>7 languages]
        MD --> Chunk[Chunker]
        HTML --> Chunk
        PDF --> Chunk
        DOCX --> Chunk
        XLSX --> Chunk
        Read --> Chunk
        Chunk --> Cmode{mode}
        Cmode -->|code| Cc[Tree-sitter boundaries]
        Cmode -->|doc| Cd[Heading boundaries]
        Cmode -->|else| Cs[Sliding window]
    end

    Syms --> Stm[IndexerStatements<br/>RAII, 14 prepared stmts]
    ER --> Stm
    Chunk --> Stm

    Stm -->|files / fts5 /<br/>symbols / headings| Main[("index.db<br/>WAL, indexer's conn")]
    Stm -->|chunks rows| Vec[("vectors.db<br/>WAL, separate conn")]

    Chunk -.->|enqueue| EQ[(embed queue)]
    EQ --> EW[EmbeddingWorker<br/>own thread, own conn]
    EW --> EM[Embedder<br/>bge-m3, 1024-dim]
    EM -->|chunk_vectors| Vec

    subgraph Q["IndexQuery (read-only)"]
        FT[FTS5 search]
        SY[symbol lookup]
        OL[outlines]
        SE[semantic KNN<br/>via vec0]
        HY[hybrid =<br/>FTS5 + vec + RRF]
    end

    Q --> Main
    Q --> Vec
    HY -.opt.-> RR[Reranker<br/>cross-encoder]
```

---

## 5. LLM Client Stack (S3.B + S4.N)

Transport, decoders, and the per-`ToolFormat` dispatch added in S4.N. The
OpenAI native path passes through unchanged regardless of which dialect is
selected -- XML extraction is purely additive.

```mermaid
flowchart TB
    Caller[AgentLoop.complete_streaming]
    Caller --> Cli[LMStudioClient<br/>: ILLMClient]

    Cli --> Tx[OpenAiTransport<br/>cpr POST + SSE +<br/>1-shot stall retry]
    Tx -->|raw SSE 'data:'<br/>payloads| Pick

    Pick{LLMConfig.<br/>tool_format}
    Pick -->|Auto| Auto[AutoToolFormatDecoder<br/>watches Qwen + Claude markers]
    Pick -->|OpenAi| OAI[OpenAiDecoder<br/>native JSON tool_calls]
    Pick -->|Qwen| QW[QwenXmlDecoder]
    Pick -->|Claude| CL[ClaudeXmlDecoder]
    Pick -->|None| NN[OpenAiDecoder<br/>tools[] omitted]

    QW -.uses.-> Xml[XmlToolCallExtractor<br/>boundary-safe<br/>partial-marker hold-back]
    CL -.uses.-> Xml
    Auto -.uses.-> Xml

    Auto --> Sink
    OAI --> Sink
    QW --> Sink
    CL --> Sink
    NN --> Sink

    subgraph Sink["StreamDecoderSink (typed events)"]
        direction LR
        T[on_text]
        R[on_reasoning]
        D[on_tool_call_delta]
        U[on_usage]
    end

    Sink --> Caller

    Caller -.token estimate.-> TC[TokenCounter<br/>~4 chars / token<br/>+4 framing per msg]

    LR[LlmRouter<br/>weak / strong<br/>S4.Q skeleton] -.future.-> Cli
```

---

## 6. Threading Model

Every long-running thread, what it owns, and how messages cross between
them. ConversationHistory has a single owner thread (the agent thread)
enforced by `assert_owner_thread`.

```mermaid
flowchart TB
    subgraph TMain["Main thread (UI / wxApp)"]
        Frame[LocusFrame]
        Bridge[WxFrontend bridge<br/>wxThreadEvent dispatch]
    end

    subgraph TAgent["Agent thread"]
        Core[AgentCore.run<br/>queue drain]
        ConvO[(ConversationHistory<br/>OWNER FENCE)]
    end

    subgraph TPump["WatcherPump thread"]
        WP[Drain efsw events<br/>-> Indexer.process_events]
    end

    subgraph TEmbed["EmbeddingWorker thread"]
        EWk[Drain queue<br/>-> Embedder<br/>-> vectors.db]
    end

    subgraph TProcs["N process-reader threads"]
        BP[BackgroundProcess.<br/>stdout drain into ring]
    end

    subgraph TLlm["cpr async (libcurl)"]
        SSE[SSE callback]
    end

    Frame -->|enqueue user msg| Core
    Core -->|FrontendRegistry events| Bridge
    Bridge -->|wxThreadEvent| Frame

    WP -->|own conn| MC[(index.db)]
    EWk -->|own conn| VC[(vectors.db)]

    BP -->|mutex + ring buffer| Reg[ProcessRegistry map]
    Reg -.->|read_output query| Core

    SSE -->|on_chunk callback| Core
    Core -->|approval wait| AGate{cv}
    AGate <-->|decision| Bridge
```

---

## 7. Context Budget & Compaction

Token accounting fires on every LLM step; soft threshold (80%) and hard
threshold (100%) both raise `on_compaction_needed`. User picks strategy;
original history is archived before replacement.

```mermaid
flowchart TB
    Step[Each AgentLoop step] --> Up[ContextBudget.<br/>update_from_usage]
    Up --> Track[track delta vs<br/>previous-turn total]
    Track --> Pct{usage / max}

    Pct -->|< 80%| Cont[Continue]
    Pct -->|>= 80%| Soft[Fire SOFT<br/>compaction event]
    Pct -->|>= 100%| Hard[Fire HARD<br/>compaction event]

    Soft --> Prompt[Frontend prompts user]
    Hard --> Prompt

    Prompt --> Strat{CompactionStrategy}
    Strat -->|B: drop oldest N| SB[Drop oldest N turns]
    Strat -->|C: summarize| SC[LLM summarizes<br/>older turns -> 1 msg]

    SB --> Arch[(Archive original<br/>history.before-*.json)]
    SC --> Arch
    Arch --> Repl[Replace history in place]
    Repl --> Foot[Footnote in new context:<br/>'archived to file X']
    Foot --> Cont
```

---

## 8. File-Edit Safety (S4.A + S4.B)

The hallucinated-edit mitigation: read-precondition, uniqueness check,
atomic apply, snapshot. `edit_file` accepts an `edits[]` array and applies
all-or-nothing.

```mermaid
flowchart TB
    Call[edit_file call] --> RT{ReadTracker:<br/>path read this session?}
    RT -->|no| R1[Refuse:<br/>'read_file first']
    RT -->|yes| Loc[Locate each old_string<br/>in current bytes]

    Loc --> Uni{All edits unique<br/>(unless replace_all)?}
    Uni -->|no| R2[Refuse:<br/>not unique]
    Uni -->|yes| Apply[Apply edits<br/>sequentially in memory]

    Apply --> Snap[CheckpointStore.snapshot<br/>writes original byte-for-byte<br/>to checkpoints/<sid>/<tid>/files/]
    Snap --> Tmp[Write new bytes to<br/>path.tmp.<rand>]
    Tmp --> Ren[Atomic rename<br/>tmp -> path]

    Ren --> MAT[FileChangeTracker.<br/>mark_agent_touched]
    MAT --> Sum[Return unified diff<br/>summary]

    R1 --> Err[(ToolResult error<br/>visible to model)]
    R2 --> Err

    subgraph Undo["/undo turn_id (later)"]
        direction LR
        US[Read manifest] --> URe[Restore each file<br/>from snapshot]
        URe --> URem[Remove created files]
        URem --> URep[Report skipped<br/>files (>1 MB)]
    end
```

---

## How to update these

When a subsystem reshape lands:

1. Update the corresponding diagram here so it stays a faithful map.
2. Keep wording short -- diagrams compete with the prose docs for
   "fastest way to orient", not for prose-level depth.
3. Don't add more than one diagram per subsystem -- this index is meant
   to be scannable in 60 seconds.
