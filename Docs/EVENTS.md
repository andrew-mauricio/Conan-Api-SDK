# Event map — what to hook to react to the game

*Portuguese translation: [EVENTOS.pt.md](EVENTOS.pt.md)*

This is the document that was missing. The API can intercept a function **by
name**, but there are 36,392 names and no hint of which one is which event.
Searching the catalogue for "chat" returns 281 results, nearly all irrelevant.

Here are the ones that matter, with **full signatures** — parameter name, type
and offset, taken from the server's own reflection data.

---

## The events, and what each one hands you

| event | function | parameters |
|---|---|---|
| **chat / command** | `ConanPlayerController::ServerSendChatMessage` | `chatData` → `ChatRpcData` (128 B) |
| **player joined** | `BaseGameMode_C::K2_PostLogin` | `NewPlayer` (PlayerController) |
| **player left** | `BaseGameMode_C::K2_OnLogout` | `ExitingController` |
| **player died** | `BasePlayerChar_C::KillPlayerCharacter` | `HitLocation`, `KillerName`, `CauseOfDeath` |
| **player respawned** | `BasePlayerChar_C::OnRespawn` | — |
| **resource harvested** | `BasePlayerChar_C::SignalXPHarvest` | `ResourceType` (FName), `ResourceNumber` (int), `IsResourcePickup` (bool) |
| **death (combat)** | `BaseBPCombat_C::OnDeath` | `HitInformation` (struct), `OriginalActorLifespan` (double) |
| **broadcast to everyone** | `ConanCheatManager::BroadcastMessage` | `Message` (FString) |
| **picked up a lore item** | `BasePlayerChar_C::PickUpLoreItem` | 2 parameters |
| **NPC died** | `BaseNPC_C::OnDeath` · `BaseHumanoidNPC_C::OnDeath` | `HitInformation`, lifespan |
| **kill a character** | `BaseBPCombat_C::KillCharacter` | 5 parameters |

---

## Chat and commands — the one everybody uses

The client sends an RPC. That `Server` prefix is Unreal's convention for "runs
on the server at the client's request", and it's exactly where a command should
be handled.

### The layout of `ChatRpcData` (128 bytes)

```
+0x000  uint64   Timestamp
+0x008  struct   UserId               (UniqueNetIdRepl)
+0x038  int64    CharacterUniqueID    <- the "uid" you see in the game log
+0x040  int64    targetUniqueId
+0x048  FString  userName             <- "Player"
+0x058  FString  Channel              <- "Global"
+0x068  FString  Message              <- "!kit", "hi"
+0x078  bool     generated
```

### The command prefix is `!`, and that was measured

**In Conan, `/` doesn't work.** The game client swallows `/command` locally and
**never sends it to the server** — no plugin, in any API, gets to see it.
Measured on the live server, with a hook logging every message before deciding
anything:

```
typed           reached the hook?   did the game process it?
"hi"            YES                 YES
"!apitest"      YES                 YES
"/apitest"      NO                  NO      <- died on the player's machine
```

`/apitest` **vanished from the player's chat**, which looked like the hook
working. It wasn't. The visible symptom is identical either way ("it
disappeared"), and only the server log tells "my hook cancelled it" apart from
"it never arrived".

People coming from other survival-server APIs expect `/`, because in those games
the client does send it. Here the choice isn't about style: it's `!` or nothing.
Use `!` or `.` — anything the client treats as ordinary text.

### Cancelling SWALLOWS the message

That's what makes a command a command: the player types `!kit`, the plugin acts,
and the text never shows up in anyone's chat.

```cpp
static Acao AoFalar(Chamada& c)
{
    if (!c.Parms || c.ParmsSize < 0x80) return Acao::Continuar;

    char texto[512];
    LerTexto(c.Parms, 0x068, texto, sizeof(texto));    // Message

    if (std::strncmp(texto, "!kit", 4) == 0)
    {
        DarKit(...);
        return Acao::Cancelar;      // never reaches chat
    }
    return Acao::Continuar;         // ordinary conversation passes through
}
ConanApi::HookProcessEvent("ServerSendChatMessage", AoFalar);
```

`LerTexto` above is a helper **inside the plugin** — it reads an `FString` at an
offset in the parameter block — not an API function. `ExemploComando` ships a
working one; copy it from there.

**Don't swallow every prefix.** The game has commands of its own, and a plugin
that hijacks everything leaves the player wondering why the game stopped
answering. Handle your prefix and return `Continuar` for the rest.

### The author of a command SEES their own command. That's normal.

Measured: the player typed `!apitest` twice and **saw both** in their chat. The
cancellation still worked:

```
the hook saw:            2 messages  -> recognised, cancelled
the server processed:    0 messages  -> the original function did NOT run
```

The game log writes `ChatWindow: ... said: <text>` for every message the server
processes. There were none. Seeing the **absence** is the proof the original
never ran.

Conan's client **displays your own message locally**, optimistically, before it
knows what the server decided. So:

  · whoever typed it  -> sees the command in their own chat (local echo)
  · everyone else     -> sees nothing (the server never relayed it)

That's the correct behaviour and nobody would guess it. If you expect the
command to disappear from your own screen, you'll conclude the hook failed —
and it didn't.

Full example: `Exemplos/ExemploComando/`.

### Answering the player

You can talk back. Build the text with the game's own allocator and call an
interface function on the controller:

```cpp
ConanApi::Call<void>(controller, "ClientHUDShowNotification",
                     ConanApi::TextoRico("You have 250 points"),
                     bool(true), bool(false));
```

`ConanApi::TextoRico` and `ConanApi::Texto` hand the game an `FText`/`FString`
the **game** allocated, which is the whole trick — see the next section for why
that matters. `Conan Shop` uses this every day in production.

---

## Player identity

This is the part where it's easy to be wrong in a way that only surfaces months
later, when somebody loses the VIP they paid for.

**The key is `ConanPlayerState.MasterAccountId`** — a `StrProperty` at offset
`0x3C0`. Confirmed with a real player, and the test that picked it was
discriminating because the wrong candidates **changed** between two sessions
(the identifiers below are anonymised examples — the measurement is real, only
the values were swapped):

```
                                   session 1                           session 2
MasterAccountId  (+0x3C0)   "A-EXAMPLE01"                       "A-EXAMPLE01"   <- stable
(field at +0x3F0)           "74315DA541274454139F5FBF0E15EC12"  "E0E5DFE3..."   <- changes
(token in PC +0x12E8)       "vwwLJ1ST.Ze43jjs..."               "bexfQXPIHJq~..." <- changes
SavedNetworkAddress (+0x350) "203.0.113.24"                     (the IP; changes with network)
PlayerNamePrivate  (+0x398)  "Player#0000"                      (the player can change it)
```

`+0x3F0` looked solid too — 32 hex digits, shaped like a GUID. Had somebody
picked it, everyone's VIP would evaporate on every relogin, and that symptom
("sometimes the VIP disappears") is one of the nastiest to diagnose.

`MasterAccountId` shows up in **four** places at once — on the character
(`BasePlayerChar_C +0xAD8`), on the `PlayerState` (`+0x3C0`), and twice on the
`PlayerController` (`+0xBC8`, `+0x1580`). A value repeated across different
objects is canonical identity, not a local cache.

The `Permission` plugin already uses this key:
`ConanPermIdDoController(pc, buf, size)`.

### What about SteamID64?

It exists, and the game log records it:

```
LogNet: Login request: userId: STEAM:7656119XXXXXXXXXX  platform: Fls
ChatWindow: Character Player (uid 110, player 7656119XXXXXXXXXX) said: hi
```

But it **was not found as text in memory** on the player objects — only
`MasterAccountId`. And note `platform: Fls`: authentication goes through Funcom
Live Services, not Steam. If you want the SteamID, the likely path is the
`UserId` (`UniqueNetIdRepl`) inside `ChatRpcData` or the `PlayerState` — **not
measured**, so not recommended until it is.

---

## How this map was obtained

Two sources, and neither of them needed guesswork.

**1. The reflection catalogue.** Filter 36,392 functions down to the classes
that belong to the game (`Conan*`, `Base*`, `FunCombat*`, `DW*`), drop the
animation, audio and interface ones, and the names become readable — candidates
fall from 281 to about a dozen.

**2. The game's own log.** It records chat with character, uid and player:

```
ChatWindow: Character Player (uid 110, player 7656119XXXXXXXXXX) said: test
```

That confirmed the processing exists and gave us the fields to look for.

**What did NOT work, for anyone who tries again:** searching the binary for the
format string `"Character %s (uid %d, player %s) said: %s"` and tracing who
references it. The string is there, exactly once, in `.rdata` — but scanning for
`lea reg,[rip+disp]` pointing at it gave **zero** references. Zero here is a
hypothesis, not a conclusion: it could be an instruction encoding we don't
decode, or a table lookup. Not worth pushing — the catalogue answered faster.

---

## Sending text INTO the game: why it needs the API

Building an `FString` that points at plugin memory and passing it to a reflected
function **crashes the server**. This was measured, with
`ConvertToAbsolutePath("test-api-xyz")`: the plugin log stops dead at the call.

The reason is structural. `ProcessEvent` **destroys the parameter block** when
the function returns — it walks `DestructorLink` and destructs every property.
`FString`'s destructor calls `FMemory::Free(Data)`, the game's allocator, on a
pointer that came off our stack. It isn't the game reading wrong: it's the game
doing the right thing with memory that was never its own.

So the string has to be allocated by the game's allocator. That's what
`ConanApi::Texto` (`FString`) and `ConanApi::TextoRico` (`FText`) do — the API
finds `GMalloc`/`FMemory::Malloc` and builds the 16-byte value
`{wchar_t* Data; int32 Num; int32 Max}` with the game's own memory. The lifetime
is the call expression, which is enough: the game copies or consumes during
`ProcessEvent` and destroys the block on return.

Same idea for `FName`, one step further: `ConanApi::Nome` goes through the
game's `Conv_StringToName`. Without it, nothing taking an `FName` was callable
at all — including `SpawnTemplateItem`, which is how items get delivered.

**A plugin cannot build any of these three on its own.** That's the point of
having them in the table.

---

## What still isn't measured

- **The real order and frequency of events.** The catalogue says the function
  exists; it doesn't say how many times per second it's called, or in what
  order. To find out, arm the wildcard hook (`api->HookProcessEventTudo`)
  **after the server has finished loading** and let it expire on its own. Armed
  during startup, it stops the world from finishing its load — measured: the
  server stalled at 4.35 GB instead of the usual 8.7.
- **`UniqueNetIdRepl`** — the network identity struct has not been decomposed.
