# Project Name

## Candidates

| Name | Pronunciation | Rationale | Concerns |
|---|---|---|---|
| **Forge** | /fɔːrdʒ/ | Workshop where things are crafted. "AI as heavy-lifter" fits the smithing metaphor. Short, strong, memorable. | Generic enough to conflict with other tools named Forge. |
| **Locus** | /ˈloʊkəs/ | Latin for "place". Every instance is bound to one locus of data. Emphasizes workspace-centricity. | Less intuitive for non-technical users. |
| **AIDE** | /eɪd/ | Acronym: AI Development Environment. Also means "helper/assistant" in plain English. Double meaning works well. | Acronym may feel forced. Many tools use "aide". |
| **Codex** | /ˈkoʊdɛks/ | Ancient knowledge books + code. Fits both coding and knowledge search use cases. | OpenAI already used Codex for their code model — may cause confusion. |
| **Sanctum** | /ˈsæŋktəm/ | Private sanctuary — fits the privacy/offline motivation strongly. | Long. Slightly heavy-handed. |
| **Atelier** | /ˌætəlˈjeɪ/ | French for workshop/studio. Sophisticated. | Non-obvious pronunciation for non-French speakers. |
| **Nexus** | /ˈnɛksəs/ | Connection point between user, AI, and data. | Very common in tech naming. |
| **Grove** | /ɡroʊv/ | Quiet, private, natural place. Gentle contrast to the "cloud". | May feel too soft for a developer tool. |

## Decision

**Locus** — chosen because:
- Unique: no significant namespace collision with existing tools
- Semantically precise: every instance is literally bound to a single locus (place/location)
- Workspace-centricity is the defining architectural feature — the name reflects that
- Two syllables, clean pronunciation, professional tone for a developer tool
- Latin origin fits the technical audience

**Config/data directory name:** `.locus/` (lives inside each workspace root)
**CLI command:** `locus` (future)

---

## Naming Conventions

- App config dir inside each workspace: `.locus/`
- Main config file: `.locus/config.json`
- Index database: `.locus/index.db`
- Session history: `.locus/sessions/`
