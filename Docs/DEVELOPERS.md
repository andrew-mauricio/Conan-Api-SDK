# Writing a plugin for Conan-Api

A plugin is a native Windows DLL that runs **inside the dedicated server
process**, server-side. Players connect with an unmodified client and download
nothing. See the [SDK README](../README.md) for the architecture and the trust
model; this document is the practical guide.

## The short version

You need **one header** and a C++ compiler. That is all.

```cpp
#include "Conan/ConanPluginApi.h"

static const ConanApiTabela* g_api = nullptr;

extern "C" ConanAcao AoFalar(ConanChamada* c)
{
    char texto[512];
    g_api->LerTextoDoJogo(c->Parms, 0x068, texto, sizeof(texto));
    if (texto[0] != '!') return CONAN_CONTINUAR;      // ordinary chat passes through

    g_api->Log("command: %s", texto);
    return CONAN_CANCELAR;                            // swallow the message
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
    g_api = api;
    g_api->Log("my plugin is up");
    g_api->HookProcessEvent("ServerSendChatMessage", AoFalar, nullptr, 100);
}
```

Build it as a DLL, put it in a folder named after your plugin inside
`Conan-Api/Plugins/`, restart the server. Done.

> **A note on identifier names.** The ABI table's function names are Portuguese
> (`ConanPluginCarregar`, `LerTextoDoJogo`, `CONAN_CANCELAR`). They are part of
> the published ABI and cannot be renamed without breaking every plugin already
> compiled against it. A glossary is at the end of this document, and every
> function is documented in English in `ConanPluginApi.h`.

---

## There is no library to link. And no source of ours to compile.

This is different from most plugin APIs, and it is deliberate.

Your plugin **receives** the API: the loader calls `ConanPluginCarregar` passing
a pointer to a table of function pointers. You call everything through `api->`.
Not one line of our runtime ends up in your binary.

**What that buys you:**

| | |
|---|---|
| **your compiler does not matter** | it is plain C. Any Visual Studio version, MinGW, clang |
| **no ABI hell** | there is no `std::string` of ours crossing the boundary to corrupt your heap |
| **an update does not force you to recompile** | new fields are appended at the end of the table and `versao` increments |
| **less of your code to go wrong** | the instruction decoder and the hook table stay on our side |

**Always check the header** before using the table:

```cpp
if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
```

A plugin compiled against a larger table, running on an older API, would read a
pointer past the end of the struct and call garbage. We never remove or reorder
fields — only append — but that check is your safety net.

---

## Versions: what has to match, and what does not

This is usually the first question, and the answer is the opposite of what most
game plugin APIs force on you.

### Your compiler does NOT have to match ours

There is no library of ours for you to link. What crosses the boundary is a
`struct` of function pointers in **plain C**, `__cdecl` — and every compiler
agrees on that.

| you use | works? | tested by us |
|---|---|---|
| Visual Studio 2026 (`cl` 19.51, v145) | yes | **yes** — it is what we validate each release with |
| Visual Studio 2017, 2019, 2022 (v141–v143) | yes | not directly; the C ABI did not change between them |
| mingw-w64 (GCC 13+) | yes | **yes** — every SDK example builds with it on every build |
| clang targeting Windows | yes | not directly |

What we test on every release is `ExemploOla` **downloaded from the published
package**, compiled with MinGW and with MSVC, and both binaries loading on the
same server. It is not a statement of intent: if either stops working, the
release does not ship.

The MSVC command we use, for you to copy:

```bat
cl /nologo /std:c++17 /O2 /EHsc /LD /MT ^
   /I "path\to\sdk\include" ^
   MyPlugin.cpp /Fe:MyPlugin.dll
```

Or don't type it: every example folder ships a **`compilar.bat`** that finds
whichever compiler you have and runs it. Open the *x64 Native Tools Command
Prompt for VS* from the Start menu, `cd` into the example, and run it. It looks
for `cl.exe` first and falls back to `g++` if you have MinGW-w64 on Windows
instead.

The `compilar.sh` beside it is the same build for Linux and WSL. It's what this
project builds with, which is why it's there — on Windows you don't need it.

Exports come out undecorated (`ConanPluginCarregar`, not
`?ConanPluginCarregar@@YA...`) because of the `extern "C"` in the header, and the
resulting DLL depends only on `KERNEL32.dll` because of `/MT`. If you see
`MSVCP140.dll` or `VCRUNTIME140.dll` in the dependency list, `/MT` did not take
effect — and the DLL will fail to load on a server running under Wine.

In APIs that ship a compiled library this is not true: the layout of
`std::string` and of vtables differs between MSVC and MinGW — and even between
MSVC versions. When they do not match, there is no clear error. It links, it
runs, and it corrupts memory on the first string that crosses the boundary. That
problem does not exist here, because nothing of ours is compiled into your
plugin.

**What does have to match:** x64 and `/MT` (or `-static-*` on MinGW). That is not
about compatibility with us — it is about the server running under Wine, where
the Microsoft runtime may not be installed.

### The TABLE version, and what it means

The header carries a number:

```c
#define CONAN_API_VERSAO 6
```

It increments when **new functions are added** — and they always go at the **end**
of the struct. We never remove or reorder a field, and that is what keeps a
plugin compiled today working tomorrow.

Declare the minimum version you need in your `PluginInfo.json`:

```json
{ "FullName": "My Plugin", "Version": "1.0.0", "MinApiVersion": 6 }
```

| situation | what happens |
|---|---|
| you use only what exists in v2, and declare `"MinApiVersion": 2` | runs on any API v2 or newer |
| you use `MensagemNaTela` (v3) but declare 2 | the loader lets it load, and **your** `api->tamanho` check is the last line of defence |
| you declare 6 and the server has v5 | the loader **refuses before loading** and says which version is missing |

Which is why the check at the top of your `ConanPluginCarregar` is not a
formality:

```cpp
if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
```

Without it, a plugin compiled against a larger table reads a pointer past the end
of the struct and calls garbage, on a server with an older API.

### If your plugin uses raw offsets, DECLARE it

This is the difference between a plugin that survives a Funcom patch and one that
silently starts reading the wrong memory.

```json
{
  "FullName": "My Plugin",
  "Version": "1.0.0",
  "MinApiVersion": 6,
  "BuildDoJogo": 24784646,
  "UsaOffsetsCrus": true
}
```

| what you declare | what the loader does when the game updates |
|---|---|
| `UsaOffsetsCrus: true` + `BuildDoJogo` | **refuses to load** and tells the admin to ask the author for a new version |
| `UsaOffsetsCrus: true` without `BuildDoJogo` | refuses — a declaration that says nothing is not a check |
| only `BuildDoJogo` | loads, and logs that the build changed |
| nothing | loads — which is correct if you only use the table |

**Why this is your problem and not ours:** the API refuses to load on a build it
does not know, deliberately. Your plugin does not get that door unless you ask
for it — and we have no way of guessing whether the `0x068` you wrote is a game
offset or a constant of your own.

The symptom of getting this wrong is the worst kind: **no error at all.** The
plugin loads, runs, and reads the neighbouring field. The server owner sees odd
behaviour weeks later with no way to connect it to your plugin.

**How to not need this:** use `OffsetDoMembro(obj, "FieldName")` (v5) instead of
the number. It resolves through reflection, on whichever build is running.

### The API version and the GAME BUILD

They are different things, and both appear in every release:

```
Conan-Api 2.7.0 — build 24784646
```

- **`2.7.0`** — the project's version
- **`build 24784646`** — the **Conan Exiles** build this API is for

The API locates engine structures in that build's memory layout. When Funcom
updates, those move, and the API **refuses to load**, deliberately, saying so in
the log.

**Does your plugin need recompiling when that happens?** Usually **no** — you talk
to the table, and the table's shape does not change. What gets rebuilt is the API.

The exception: if your plugin uses a **raw game offset** (like the `0x068` of
`ChatRpcData` in the chat example), that number may have moved, and you need to
check. The more you use table functions instead of direct offsets, the less a
game update affects you.

---

## Visual Studio

1. **New Project** → *Dynamic-Link Library (DLL)*
2. **Properties → C/C++ → General → Additional Include Directories**: point at
   the `include` folder of the SDK you downloaded
3. **Properties → C/C++ → Code Generation → Runtime Library**: `/MT`
   (Multi-threaded), **not** `/MD`
4. **Platform: x64.** The server is 64-bit; a 32-bit DLL will not load and the
   error will not say why.

**Why `/MT` and not `/MD`:** `/MD` makes your DLL depend on the Microsoft runtime
being installed on the machine. The server runs under Wine, in a container where
that runtime may not exist — and the symptom is `LoadLibrary` failing with a
generic code. With `/MT`, the runtime goes inside your DLL.

There is nothing to link. No `.lib`, no `.a`, no `.cpp` of ours added to your
project.

## mingw-w64 (Linux, macOS or Windows)

```bash
x86_64-w64-mingw32-g++ -std=c++17 -O2 -shared \
    -I path/to/the-sdk/include \
    -o MyPlugin.dll MyPlugin.cpp \
    -static-libgcc -static-libstdc++
```

`-static-libgcc -static-libstdc++` for the same reason as `/MT`: do not depend on
a compiler DLL that will not be on the server.

---

## From nothing to a running plugin

```bash
cp -r Exemplos/ExemploOla MyPlugin
cd MyPlugin
mv ExemploOla.cpp MyPlugin.cpp
sed -i 's/ExemploOla/MyPlugin/g' compilar.sh
./compilar.sh
```

That produced `MyPlugin.dll`. To install, copy the **folder** `MyPlugin/` (with
the DLL inside) into the server's `Conan-Api/Plugins/` and restart.

**Want the plugin in your own project, outside the SDK folder?** You can — just
say where `include` is:

```bash
CONAN_SDK_INCLUDE=/path/to/Conan-Api-SDK-v2.5.0/include ./compilar.sh
```

`compilar.sh` walks up the directory tree looking for the header on its own, so
while the plugin lives **inside** the SDK (at any depth) it finds it. Moved
outside, it does not guess: it stops and prints that line, instead of letting the
compiler complain about an `#include` that looks wrong and is not. The folder
name and the DLL name must match — that is how the loader picks which one to
load.

## The structure of a plugin

Each plugin is **one folder**, with everything inside:

```
Conan-Api/Plugins/MyPlugin/
   MyPlugin.dll         <- the loader looks for this name first
   PluginInfo.json      <- optional; if present, the log shows name and version
   config.json          <- optional, and YOURS: CaminhoConfig("MyPlugin")
   mydatabase.db        <- CaminhoDados("MyPlugin", "mydatabase.db")
```

The user installs by dragging the folder in. Uninstalls by deleting it.

### Only the DLL is required

| file | required? | who reads it |
|---|---|---|
| `MyPlugin.dll` | **yes** — the only thing the loader needs | the loader |
| `PluginInfo.json` | no | the loader, **if** present |
| `config.json` | no | **your plugin**; the API never opens it |

A plugin with nothing but the DLL loads and works. `Cartografo` and
`GravadorDeEventos` in this project run exactly like that:

```
  [ok] Cartografo   [sem PluginInfo.json]
```

`config.json` has no format imposed by us. The API only hands back the **path**,
through `CaminhoConfig("MyPlugin")` — the file may be JSON, may have another
name, may not exist at all.

**Without `PluginInfo.json` you lose four things:**

- the plugin's name and version in the log (only the folder name shows)
- `MinApiVersion` — nothing refuses your plugin on an API that is too old
- `Dependencies` — nothing guarantees Permission loads before you
- `BuildDoJogo` / `UsaOffsetsCrus` — the plugin loads after a game update even
  when it should not

For a plugin only you use, none of that matters. For one you **publish**, all of
it does.

**If there is more than one `.dll` in the folder**, the loader uses the one named
after the folder. With only one, it uses that. With two and none matching the
folder name, it **refuses** and logs which one to rename — picking "the first"
would be an invisible decision that changes with filesystem ordering.

**Store everything through the API**, never by relative path:

```cpp
const char* db = g_api->CaminhoDados("MyPlugin", "mydatabase.db");
```

A relative path resolves against the **server's** working directory, not your
folder. A plugin once wrote 9 MB per boot into the wrong place that way.

---

## Using Permission (VIP, groups, permissions)

`Permission` is the package's default plugin. It stores who has what, and other
plugins query it through a C ABI — **with nothing to link**, via
`GetProcAddress`:

```cpp
#include "Conan/ConanPermission.h"      // header-only, ~100 lines

char id[64];
if (ConanPermIdDoController(controller, id, sizeof(id)) > 0)
    if (ConanPermTem(id, "myplugin.kit.daily", /*if_absent=*/0) == 1)
        GiveKit(controller);
```

**If Permission is not installed**, the functions return the `if_absent` value
you passed, and your plugin keeps working. It degrades; it does not break.

**Query at the moment of USE, not at load time.** Asking inside
`ConanPluginCarregar` can happen before Permission has come up, and you conclude,
in good faith, that nobody installed it. (It happened here: `ExemploVip`
announced "Permission is not installed" on a server where it was.)

### Permission may be on SQLite **or on MySQL**, and you cannot tell

The server owner chooses where the data lives, with one line in their
`config.json` (`"Database": "sqlite"` or `"mysql"`). That is their decision, not
yours, and your plugin **should hold no opinion and no code about it**: the ABI is
identical either way — same names, same types, same semantics.

What it does oblige you to handle is **one thing**, and it is the one usually
missed:

> **"Absent" is not only "not installed". It is a state that comes and goes while
> the server runs.**

With SQLite, if Permission loaded, it answers — the file is local and does not
drop. With **MySQL**, the database is on another machine, and the owner's network
is neither your problem nor theirs: it goes down. When it does,
`ConanPermissionObterApi` returns `nullptr` — **absent** — and the header helpers
return the `if_absent` you passed. When the connection returns (Permission
retries on its own, in the background), the next call answers normally again.

Which means the same player may answer `1` one minute, `if_absent` the next, and
`1` again after — **with nothing wrong in your plugin**.

```cpp
// RIGHT: decide at the point of use, and the absent value is YOUR conscious choice
if (ConanPermTem(id, "myplugin.kit.daily", /*if_absent=*/0) == 1)
    GiveKit(controller);

// WRONG: caches the answer forever. If you happen to ask during a MySQL outage,
// this player has no VIP until the server restarts.
if (!asked) { isVip = ConanPermTem(id, "vip.kit", 0); asked = true; }
```

**Choose `if_absent` by the cost of being wrong, not by reflex:**

| what the permission unlocks | sensible `if_absent` | why |
|---|---|---|
| a kit, a teleport, a VIP bonus | `0` (deny) | denying for 30 s annoys; handing it to everyone during an outage cannot be undone |
| a destructive admin command (`!wipe`, `!ban`) | `0` (deny) | always |
| something that merely *hides* cosmetic information | `1` (allow) | the cost of being wrong is zero |

Never treat `-1` as "denied". Calling `a->tem()` directly on the table returns
`-1` for "I do not know"; the header helpers already translate that into your
`if_absent`, which is why they exist. Treating `-1` as `0` by hand takes VIP away
from someone who paid for it, precisely in the minute the owner's database is
having trouble.

### What you **must not** do

**Do not open `permission.db` yourself.** It is tempting — there is a SQLite file
right there, and nothing technically stops you (there is no boundary between
plugins; see `_fronteira` in Permission's `config.json`). But:

1. **the file may not exist.** If the owner is on MySQL there is no `.db` at all,
   and your plugin now works on only half the servers;
2. **the schema is internal and changes without notice.** The ABI is the contract;
   the tables are not;
3. **you would not see the cache.** Permission answers from a snapshot published
   in memory, and writes through its own worker. Reading the file from outside
   gives you a picture out of step with the one everyone else is using.

**Do not store anything of yours inside Permission's database**, on either
backend. Use `g_api->CaminhoDados("YourPlugin", "yourdb.db")` and have your own.

**Do not call the ABI expecting it to hit the network.** It does not: a query
reads the in-memory snapshot, is cheap, and can be called from the game loop.
What talks to MySQL is Permission's worker thread, never yours and never the
game's — that is how a database being down was kept from turning into
disconnected players. If a query of yours ever seems slow, the owner's database
is not the problem.

**If you distribute a plugin that uses Permission**, say in your README that it
works with both — and test with Permission **absent** at least once. It is the
path nobody exercises, and it is the one that runs on the owner's worst day.

---

## Answering in the first second: register early

The server accepts players **before** the world finishes building. In that window
reflection does not exist yet, so a plugin activated only afterwards leaves
whoever typed an early command without an answer.

For that there is a second, optional entry point:

```cpp
extern "C" __declspec(dllexport)
void ConanPluginRegistrar(const ConanApiTabela* api)   // BEFORE the world
{
    ConanApi::UsarTabela(api);
    g_id = api->HookProcessEvent("ServerSendChatMessage", AoFalar, nullptr, 100);
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)    // AFTER the world
{
    ...
}
```

`HookProcessEvent` called from `Registrar` **queues** and returns a valid id
immediately. The API arms the hook the instant the world comes up — before any
plugin is activated.

**What you must NOT do in `Registrar`:** touch a game object. There is no world,
no reflection, and `FindClass`/`FindObjects` return nothing. If you need the
world, the place is `Carregar`.

**You do not have to export `Registrar`.** Without it the plugin behaves exactly
as before — hooks go in when `Carregar` runs. `Registrar` only shortens the
window.

---

## From chat to the player: the path every plugin needs

This is the jump missing from almost every API, and without it the 9,247 classes
are worth nothing: **someone typed something — who was it, and where are they?**

```cpp
// The helper that repeats in every plugin: resolve the offset BY NAME,
// read the pointer, and refuse anything not readable.
static void* MembroPonteiro(void* obj, const char* nome)
{
    if (!obj) return nullptr;
    const int32_t off = g_api->OffsetDoMembro(obj, nome);
    if (off < 0) return nullptr;

    void* p = nullptr;
    if (g_api->LerMembro(obj, uint32_t(off), &p, sizeof(p)) <= 0) return nullptr;
    if (!p || !g_api->Legivel(p, 8)) return nullptr;
    return p;
}

extern "C" ConanAcao AoFalar(ConanChamada* c)
{
    void* controller = c->Obj;              // 1. WHO spoke

    char nome[128] = "";                    // 2. the NAME lives on the PlayerState
    if (void* ps = MembroPonteiro(controller, "PlayerState"))
    {
        const int32_t off = g_api->OffsetDoMembro(ps, "PlayerNamePrivate");
        if (off >= 0) g_api->LerTextoDoJogo(ps, uint32_t(off), nome, sizeof(nome));
    }

    void* corpo = MembroPonteiro(controller, "Character");   // 3. the CHARACTER
    if (!corpo) corpo = MembroPonteiro(controller, "Pawn");

    struct { double X, Y, Z; } pos{};       // 4. the POSITION, via the game's function
    g_api->ChamarFuncao(corpo, "K2_GetActorLocation", nullptr, nullptr, 0,
                        &pos, sizeof(pos));

    g_api->MensagemParaJogador(nome, "found you");
    return CONAN_CANCELAR;
}
```

**No hardcoded offsets.** Running on this server, `OffsetDoMembro` returned
`PlayerState → 0x308`, `Character → 0x350`, `Pawn → 0x340`. Writing those numbers
into your code works today and reads the neighbouring field after the next patch.

The position comes from the **function** rather than the field on purpose:
`RelativeLocation` is replicated, and reading the raw field hands you the value
from before the last replication.

### With no hook at all

When the starting point is not a player speaking — a scheduled task, an admin
command:

```cpp
void* pcs[64];
int n = g_api->FindObjects("ConanPlayerController", pcs, 64, /*includeSubclasses=*/1);
```

From there it is the same path.

**Do not hold those pointers between calls.** The game's garbage collector
destroys objects and reuses addresses; `Legivel` would still say yes, because the
page is still mapped, and you would act on something else entirely.

Full example in `Exemplos/ExemploJogador`.

---

## What your hook receives

The guide uses `c->Parms` in the examples, but the struct carries more — and
`c->Obj` is what most plugins are missing:

```c
typedef struct ConanChamada {
    void*    Obj;         // the UObject that received the call  <- who, in game
    void*    Func;        // the UFunction
    void*    Parms;       // parameter block (may be NULL)
    uint32_t ParmsSize;
    int32_t  NomeIndice;  // FName.ComparisonIndex — O(1) comparison
    int32_t  NomeNumero;
} ConanChamada;
```

In a `ServerSendChatMessage` hook, `c->Obj` is the `ConanPlayerController` of
whoever spoke. That is what you pass to `MensagemNaTela` and to `Permission`.

**Do not hold `c->Obj` between calls.** The garbage collector may destroy the
object and reuse the address; `Legivel` would still return 1, because the page is
still mapped, and you would act on something else. Fetch it again in each hook.

## Counting who is online

```cpp
void* found[128];
int n = g_api->FindObjects("ConanPlayerController", found, 128, /*includeSubclasses=*/1);
```

That last parameter is not decoration: without it, subclasses are left out and
the count comes back smaller than reality. And `FindObject` (singular) answers a
different question — it returns **the first one**, so with two players on the
server you would see one.

## HookProcessEvent's return value: zero is FAILURE

```cpp
uint32_t id = g_api->HookProcessEvent("ServerSendChatMessage", AoFalar, nullptr, 100);
if (id == 0)
    g_api->Log("could not hook chat");   // the reason is already in the log
```

It returns the **hook id**, not an error code. `0` means it failed — the opposite
of the `0 == ok` reflex most of us have in C. Keep the id if you intend to call
`RemoverHook`.

---

## The contract

```cpp
extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api);   // required

extern "C" __declspec(dllexport)
void ConanPluginDescarregar(void);                     // optional
```

**Do no work in `DllMain`.** Windows holds a global loader lock there, and
calling almost anything deadlocks the whole process. Do everything in
`ConanPluginCarregar`.

---

## The whole game, with real signatures

Besides the table, the package ships **`ConanSDK.h`**: **9,247 Conan classes**
with the members and functions the game's reflection declares. It is not a list
of names: **~89% of the 38,340 functions have a complete signature** — type and
name of every parameter, checked against the running server with the world
loaded, on build `24784646`.

```cpp
#include "Conan/ConanSDK.h"

void ConanPluginCarregar(const ConanApiTabela* api)
{
    ConanApi::UsarTabela(api);          // <- required, see below
    ...
    cm->TeleportPlayer(1000.0f, 2000.0f, 300.0f);
    FVector where = actor->K2_GetActorLocation();
    cm->CheatSpawnItem(TemplateId, quantity);
}
```

**`ConanApi::UsarTabela(api)` is required.** The header has no way of obtaining
the table on its own — it arrives in your `ConanPluginCarregar`. Without that
line every SDK call silently becomes a no-op; which is why the first one warns on
`stderr` instead of leaving you to hunt for the reason.

An **output** parameter becomes a reference, and the API copies the value back:

```cpp
FHitResult hit{};
actor->K2_SetActorLocation(destination, false, hit, true);
```

Output text becomes `char*` plus capacity — **decoded**, not the game's pointer
(which dies when the call returns):

```cpp
char name[128];
someone->GetDisplayName(name, sizeof(name));
```

An output list becomes pointer, capacity and count, with the **elements** copied:

```cpp
AActor* found[32]; int howMany = 0;
lib->SomeActorSearch(..., found, 32, howMany);
```

### `FName` as input: use `ConanApi::Nome("...")`

An `FName` **is not a string**: it is two integers pointing at an entry in that
process's name pool. You cannot invent those numbers — and inventing is worse
than getting them wrong, because `{0,0}` is the name `None` and any other pair is
*some* valid name, just not yours. The game accepts it and does something else,
without a single error.

```cpp
// deliver an item to the character: Context is an FName
character->SpawnTemplateItem(10001, ConanApi::Nome("myshop"),
                             100, 1.0f, 0.0f, true);

// put an item into an inventory
inventory->AddItemTemplate(10001, -1, ConanApi::Nome("myshop"),
                           100, false, 1.0f, 0.0f);
```

`ConanApi::Nome` asks the game itself (`Conv_StringToName`), which **creates** the
name in the pool if it does not exist yet — which matters when you invent a
context of your own that no part of the game would use. The result is memoised,
so calling it in a loop costs nothing.

> This landed on 20/08/2026 and unlocked what had been out of reach: without that
> bridge, **no** function taking an `FName` parameter was callable — including the
> two above, which are *how you hand an item to a player*. A shop was impossible
> to write, and the reason showed up nowhere.

**You link nothing.** The whole SDK routes through the table — which is why it
behaves identically under MSVC, MinGW and clang. If your project ever asks for a
`libconanapi.a`, something is wrong: there is no library of ours to link.

The remaining ~11% come out as untyped generic templates, deliberately: they are
types that **carry ownership of engine memory** (`TArray<FString>`, `TMap`,
multicast delegates). Passing them by value would duplicate pointers and someone
would free twice. We prefer an untyped template to a signature that corrupts.

---

## "It ran" is not "it worked"

Read this before you trust any call you make. It cost us a working-looking
plugin that lied to players with total confidence.

`UltimaChamadaExecutou()` answers exactly one question, truthfully: **was the
UFunction dispatched?** It cannot answer *did the body do any work?*, and
nothing in the API can. Those are different questions, and a great many Conan
functions answer the first with yes and the second with no:

- the whole `ConanCheatManager` surface returns early for a player who is not an
  admin — the call dispatches, and nothing happens;
- a setter whose precondition fails dispatches and leaves the field alone.

We shipped a command that called `ConanCheatManager::LearnAllFeats()`, saw the
sentinel say yes, and told the player everything was unlocked. The knowledge
window still showed padlocks and *not enough points*. The log of that run:

```
[engrams] first run: cheat manager=ConanCheatManager  admin=no
[engrams] unlocked for <player> (feats + recipes)
```

Both statements were true about the calls and false about the world.

**The rule: anything that matters is confirmed by reading the resulting state.**
The game usually hands you the reader right next to the writer —
`IsFeatPurchased` beside `ServerForceLearnFeat`, `LerBit` beside `EscreverBit`.
Use it, count the confirmations, and report those numbers. A count of calls made
is not a result.

---

## Granting knowledge: feats and recipes

A worked example, because `ConanSDK.h` holds 9,247 classes and knowing that a
thing exists is useless if you cannot find it.

Knowledge in Conan is **two separate systems**, and unlocking one leaves the
player stuck:

| | what it is | where it lives |
|---|---|---|
| **Feats** | the knowledge tree, the part that costs points | `UProgressionSystem`, per character |
| **Recipes** | the individual craftable entries | `UConanCharacter.UnlockAllRecipes`, a replicated flag |

### The pieces, and their names in the SDK

```cpp
UConanCharacter::GetProgressionSystem()                    // the component
UProgressionSystem::ServerForceLearnFeat(id, fromNPC,
                                         suppressReports,
                                         bUpdateJourneys)  // grant, forced
UProgressionSystem::IsFeatPurchased(id) -> bool            // the proof
UConanCharacter::UnlockAllRecipes()                        // BitRef
UConanCharacter::OnRep_UnlockAllRecipes()                  // tell the game
```

**`ServerForceLearnFeat` versus `ServerPurchaseFeat`.** The purchase path is the
shop, and it checks what you would expect — `CheckFeatCost`, `CheckFeatLevel`,
`CheckFeatPrerequisite`. The forced path is how the game itself grants a feat
when a teacher NPC or a quest gives one: **no points spent, no level
requirement, no prerequisite walk**, and no admin anywhere in it. If you are
writing "unlock everything", this is your function — and it is the game doing
the work rather than you writing over its data.

### Where the list of feats comes from — and where it does NOT

**Not from the object array.** This is the trap, and it costs a whole test cycle
to discover, so it is written down here instead.

`UFeatItem` derives from `UGameItem`, so it is tempting to walk
`FindObjects("FeatItem", ...)` and read `TemplateId` off each one. That code
runs, finds objects, and reports a flawless result. It is still wrong: **the
game only instantiates a `FeatItem` for a feat the character already knows.**
Measured on a live server with a level-26 character:

```
feats: 98 walked · 0 learned now · 98 already had · 0 refused
```

98 objects, 98 already owned, nothing refused — a perfect score against the
wrong population. The knowledge window still showed padlocks, because the
hundreds of feats the player did *not* have were never objects to begin with.

**The canonical source is the DataTable**, the same place the game reads. In a
Conan `DataTable` the **row names are the template ids**, and the table holds
every feat in the build whether or not anybody ever learned it:

```cpp
struct FNameCru { int32_t indice; int32_t numero; };

// 1. find the table among the ~880 the world loads
void* tables[8192];
const int nt = g_api->FindObjects("DataTable", tables, 8192, /*subclasses=*/1);
// ...pick the one whose NomeDoObjeto() is "FeatTable", skipping "Default__" (the CDO)

// 2. its row names are the ids
void* lib = g_api->GetDefaultObject("DataTableFunctionLibrary");
std::vector<FNameCru> rows(65536); int n = 0;
ConanApi::CallSaida(lib, "GetDataTableRowNames", table,
                    ConanApi::ParaForaLista(rows.data(), 65536, n));

char buf[64];
for (int i = 0; i < n; ++i) {
    g_api->NomeDeFName(rows[i].indice, buf, sizeof(buf));
    const long id = std::strtol(buf, nullptr, 10);
    ...
}
```

The row struct is `FeatTableRow`, and it is in `ConanStructs.h` with its layout
checked by `static_assert` — so you can read the columns too, the same way the
item catalogue does with `GetDataTableColumnAsString`.

**Print the near misses.** When the table is not found, log every DataTable
whose name contains "feat" rather than just failing: a modded server can name it
differently, and that list is the only thing that will tell you.

If you want the cost, level and prerequisites — to build a knowledge *shop*
rather than a giveaway — a live `UFeatItem` answers `GetFeatCost()`,
`GetLevelRequirement()`, `GetPrerequisiteFeat()` and `GetRewardRecipe()`. Just
remember which question each source answers: the object array tells you what a
character **has**, the table tells you what **exists**.

### The shape that works

```cpp
void* prog = ConanApi::Call<void*>(pawn, "GetProgressionSystem");

for (int i = 0; i < n; ++i)
{
    int32_t id = 0;
    const int32_t off = g_api->OffsetDoMembro(items[i], "TemplateId");
    if (off < 0) continue;
    g_api->LerMembro(items[i], uint32_t(off), &id, sizeof(id));
    if (id <= 0) continue;                       // class default object, holes

    if (ConanApi::Call<bool>(prog, "IsFeatPurchased", id)) { ++already; continue; }

    ConanApi::Call<void>(prog, "ServerForceLearnFeat",
                         id, false, /*suppressReports=*/true, false);

    // The only line that decides anything.
    ConanApi::Call<bool>(prog, "IsFeatPurchased", id) ? ++learned : ++refused;
}
```

`suppressReports=true` matters: there are hundreds of feats, and each one would
otherwise pop its own notification at the player.

For the recipes half, set the flag, run the game's own reaction, then read the
bit back:

```cpp
const int32_t off = g_api->OffsetDoMembro(pawn, "UnlockAllRecipes");
g_api->EscreverBit(pawn, uint32_t(off), 1, 1);
ConanApi::Call<void>(pawn, "OnRep_UnlockAllRecipes");
const bool actuallySet = g_api->LerBit(pawn, uint32_t(off), 1) > 0;   // report THIS
```

`OnRep` is what runs the game's reaction to the change; on the server nobody
calls it for you, and without it the server believes one thing while the
player's screen shows another. Writing the **bit** rather than the byte costs
nothing and survives a patch that packs the flag beside another one.

### Not every feat can be granted, and that is correct

The base `FeatTable` is **not** just the base game. It carries every DLC's feats
too, tagged in `FeatTableRow.DLCPackage`, and the game refuses to grant one that
belongs to a DLC the account does not own.

This is worth knowing before you spend an afternoon debugging it. Two accounts,
one server, identical code, the same 2346-row table:

```
account A    913 learned · 101 already had · 1332 refused
account B    871 learned · 346 already had · 1129 refused
```

**Different refusals from the same table** is what tells you it is entitlement
and not your bug — a defect in your loop would refuse the same rows for both.
The game's knowledge window agrees: those padlocks read *DLC missing*, not *not
enough points*.

Read the `DLCPackage` column and group your refusals by it. "1332 refused" is a
number nobody can act on; "1332 refused, all of them DLC" is an answer.

`ConanCheatManager::SetBypassEntitlements` exists and would step over the check.
**Don't.** DLC is content Funcom sells, and a plugin that gives it away is not a
plugin any server owner should install by accident.

---

## Talking to the player

This has existed since table **v3**, and there are three distinct routes:

```cpp
g_api->MensagemParaTodos("Server restarts in 5 minutes.");
g_api->MensagemParaJogador("PlayerName", "Kit delivered. Come back in 24h.");
g_api->MensagemNaTela(playerController, "Welcome!", 8.0f);
```

**Note what each one addresses** — this is the easiest mistake to make:

| function | takes | where it comes from |
|---|---|---|
| `MensagemParaTodos` | nothing | — |
| `MensagemParaJogador` | **the name**, `const char*` | `userName`, offset `0x048` of chat |
| `MensagemNaTela` | **the controller**, `void*` | `c->Obj` in a hook, or `LerParm` at login |

Passing the controller to `MensagemParaJogador` does not compile (the type saves
you). But in a chat hook you have the `ChatRpcData`, and that is where the name
comes from.

`MensagemParaJogador` returns **0 if the player is not connected** — handle that,
because they may have left between the command and the answer.

### What remains forbidden

**Building your own `FString` and passing it to the game.** That **crashes the
server**: `ProcessEvent` destroys the parameter block on return and calls *the
game's* allocator on a pointer from your stack. That was measured here, not
assumed — and it is precisely why the functions above exist: they ask the game to
allocate. If you need to pass text to a game function yourself, use
`CriarTextoDoJogo` (v4), which returns 16 bytes the game owns.

**Hooking an arbitrary address.** `HookFuncao` **refuses** roughly 32% of
functions, with the reason in `TextoRecusa`. A refusal is not a failure: it is the
API declining to do something that would execute half an instruction one day,
corrupting memory hours later with no readable error.

---

## Installing without taking the server down

The server owner does not need to restart to install your plugin:

```
1. copy the folder MyPlugin/ into Conan-Api/Plugins/
2. create the empty file Conan-Api/CARREGAR-NOVOS
```

Within 3 seconds the loader picks it up, runs the same checks as a normal load,
and calls your `ConanPluginCarregar`. The log says what happened:

```
[novos] [ok] MyPlugin carregado SEM reiniciar o servidor.
```

**Replacing the version of an already-loaded plugin still requires a restart.**
That is not a temporary limitation: once loaded, your plugin has hooks armed and
possibly scheduled tasks pointing into your DLL. Unloading it with any of those
live would make the game jump into unmapped memory later, far from the cause. We
would rather ask for a restart than ship that.

**What this changes for you:** your `ConanPluginCarregar` may run with the world
**already full** — players connected, objects alive, other plugins working. If it
assumes it is at startup (that the player list is empty, that nothing has been
initialised yet), it will be surprised. Write it to work at both moments.

---

## When it does not work: where to look

Two files answer almost everything, both in `Conan-Api/Logs/`:

| file | what it tells you |
|---|---|
| `ConanLoader.log` | which plugins the loader **saw**, which it **refused**, and why |
| `ConanApi.log` | what plugins **wrote** with `Log()`, plus runtime warnings |

The loader writes a verdict per plugin with the reason attached. Read the whole
line before anything else:

```
  [ok] MyPlugin  "My Plugin"  v1.0.0  api>=6
  [x]  Other — exige API versao 7; esta instalacao e' a 6.
  [x]  Third — abriu, mas NAO exporta ConanPluginCarregar().
```

> Loader and runtime log messages are currently written in Portuguese.
> Translating them is a code change and is tracked separately from this guide.

### The mistakes that come up most

**The DLL does not open.** Almost always architecture (built x86 instead of x64)
or a missing Microsoft runtime — `/MD` instead of `/MT`. The log gives the
Windows error code; `193` is "not a valid Win32 application", which in practice
means 32-bit.

**It opened, but nothing happens.** Check that the folder name and the DLL name
match, and that you exported `ConanPluginCarregar`. Under MSVC, without
`extern "C"` the name comes out decorated and the loader will not find it:

```bat
dumpbin /exports MyPlugin.dll | findstr ConanPlugin
```

You should see `ConanPluginCarregar`, not `?ConanPluginCarregar@@YAXPEBU...`.

**It loaded, but `ConanSDK.h` calls do nothing.** `ConanApi::UsarTabela(api)` is
missing. The warning goes to the server's `stderr`, which under Wine lands in the
game's log, not ours.

**A function returns `false` and you cannot tell whether it ran.** Use
`UltimaChamadaExecutou()`: the game filters calls on a CDO, on a Blueprint
template and on an uninitialised actor, and in those cases the return value comes
from a zeroed block. Without that signal, "the function said no" and "the function
did not run" are the same `false`.

**You wrote to a field and the client does not see it.** The field is replicated.
Ask first:

```cpp
const int32_t off = api->OffsetDoMembro(obj, "Field");
if (api->EhReplicado(obj, off) == 1) { /* call the game's function, do not write */ }
```

There are 1,222 of the 36,210 members in this build. `EscreverMembro` warns once
per field, with the name.

---

## Before you publish

- [ ] builds x64, with `/MT` (MSVC) or `-static-*` (MinGW)
- [ ] the folder is named after the plugin, and so is the DLL
- [ ] everything it writes goes through `CaminhoDados("YourPlugin", ...)`
- [ ] it checks `api->tamanho` before using the table
- [ ] `DllMain` does nothing
- [ ] if it uses Permission, it queries at the point of use and degrades when absent
- [ ] it ran on a real server — a 200 from `curl` does not prove a screen, and a
      unit test does not prove the real path

---

## Glossary: ABI names

The table's identifiers are Portuguese, because they are part of the published
ABI and renaming them would break every plugin already compiled. Their meaning:

| ABI name | English |
|---|---|
| `ConanPluginCarregar` | plugin entry point, called after the world is up |
| `ConanPluginRegistrar` | optional early entry point, before the world |
| `ConanPluginDescarregar` | called at unload |
| `ConanApiTabela` | the function table |
| `ConanChamada` | the hook call context |
| `ConanAcao` / `CONAN_CONTINUAR` / `CONAN_CANCELAR` | hook verdict: continue or cancel |
| `Log` | write to `ConanApi.log` |
| `Legivel` | is this pointer readable |
| `LerMembro` / `EscreverMembro` | read / write a field by offset |
| `LerBit` / `EscreverBit` | read / write a bitfield |
| `OffsetDoMembro` | resolve a field offset **by name** |
| `EhReplicado` | is this field replicated |
| `ChamarFuncao` / `ChamarFuncaoEx` | call a game function by name |
| `HookProcessEvent` / `RemoverHook` | install / remove a hook |
| `AgendarNaThreadDoJogo` | schedule work on the game thread |
| `LerTextoDoJogo` / `CriarTextoDoJogo` | read / allocate an engine string |
| `MensagemParaTodos` / `MensagemParaJogador` / `MensagemNaTela` | messaging |
| `CaminhoConfig` / `CaminhoDados` / `CaminhoRaiz` | paths owned by your plugin |
| `UltimaChamadaExecutou` | did the last call actually execute |
| `NumObjects` / `GetObjectByIndex` / `FindObject` / `FindObjects` | object lookup |
| `GetDefaultObject` / `DescendeDe` / `NomeDoObjeto` | class helpers |
