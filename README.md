<p align="center">
  <img src=".github/imagens/conan-header.jpg" alt="Conan Exiles Enhanced native server plugin SDK">
</p>

<p align="center">
  <a href="README.md"><img src=".github/imagens/bandeiras/us.png" alt="English" height="13">&nbsp;<b>English</b></a>
  &nbsp;&nbsp;&middot;&nbsp;&nbsp;
  <a href="Docs/README.pt.md"><img src=".github/imagens/bandeiras/br.png" alt="Portugu&ecirc;s" height="13">&nbsp;Portugu&ecirc;s</a>
  &nbsp;&nbsp;&middot;&nbsp;&nbsp;
  <a href="Docs/README.es.md"><img src=".github/imagens/bandeiras/es.png" alt="Espa&ntilde;ol" height="13">&nbsp;Espa&ntilde;ol</a>
</p>


# Conan-Api SDK, write native server-side plugins for Conan Exiles Enhanced

**This is the developer SDK for [Conan-Api](https://github.com/andrew-mauricio/Conan-Api),
a server-side plugin framework for privately operated Conan Exiles Enhanced
dedicated servers.** Plugins you build with it are ordinary Windows DLLs that run
inside the dedicated server process, server-side only. Players connect with an
unmodified client and download nothing.

If you have written plugins for **ArkApi** or **AsaApi**, the model will be
familiar. See [Coming from ArkApi or AsaApi](#coming-from-arkapi-or-asaapi) for
what is the same and what is different.

You need **one header** and a C++ compiler. There's no library to link, no code
of ours to compile alongside, and no project file to configure.

---

## Coming from ArkApi or AsaApi

| | ArkApi / AsaApi | Conan-Api |
|---|---|---|
| entering the server process | proxy DLL | **same** (`winmm.dll` import forwarding) |
| plugin ABI | C++ — ties you to their compiler | **plain-C function table — any compiler** |
| calling a game function | by address | **by name, through engine reflection** |
| field offsets | baked into the plugin | **resolved at runtime, by name** |
| permissions | Permissions | Permission |
| where the plugin runs | dedicated server process | **same** |

The practical consequence of rows two through four: a plugin compiled with MSVC
and one compiled with MinGW both load, and a plugin that never hardcodes an
offset keeps working across game updates that move memory layout around.

---

## Architecture

```
Conan Exiles Enhanced dedicated server  (ConanSandboxServer-Win64-Shipping.exe)
        │
        ▼
Conan-Api runtime layer          build verification, engine-structure discovery,
        │                        ProcessEvent hook, game-thread scheduler
        ▼
ABI function table               plain-C struct of function pointers.
        │                        Versioned; a plugin declares the minimum it needs.
        ▼
C++ SDK headers                  ConanPluginApi.h (the table)
        │                        ConanBase.h      (typed wrappers, ConanApi::Call)
        │                        ConanSDK.h       (generated: 9,247 classes)
        ▼
Your plugin                      a DLL in Plugins/<Name>/, with PluginInfo.json
```

**The ABI table is the contract.** Your plugin receives a pointer to a C struct
of function pointers in its entry point, and every interaction with the engine
goes through it. Nothing of ours is linked into your binary. That's what makes
the compiler irrelevant, and it's also why a runtime built for a newer game
build can serve a plugin compiled months earlier: the table's shape is versioned
and additive.

**The scheduler exists because the engine isn't thread-safe.** Work you hand to
`AgendarNaThreadDoJogo` runs on the elected game thread, not on whichever thread
your callback happened to be on. Calling engine functions from another thread is
the fastest way to corrupt a running server.

---

## Runtime reflection: how calling by name works

Unreal Engine keeps reflection metadata about its own types inside the running
process, class names, function names, parameter names and types, property
offsets. The API reads that metadata and uses it to locate and invoke game
functions.

That's what lets a plugin write this:

```cpp
character->SpawnTemplateItem(10001, ConanApi::Nome("myshop"), 100, 1.0f, 0.0f, true);
```

instead of an address and a hand-built parameter block. `SpawnTemplateItem` is
looked up by name on the object's actual class, the parameter block is laid out
from the reflected signature, and the call goes through `ProcessEvent` — the same
path the engine itself uses.

**Measured on build `24784646`, with the world loaded:**

| | measured |
|---|---|
| Conan classes visible through reflection | **9,247** |
| reflected functions | **38,340** |
| of those, with a complete typed signature | **~89%** |
| class members catalogued | **36,210** |
| of those, replicated | **1,222** |

These numbers describe **one specific build** and are reproducible with the
tooling in this repository. They aren't a promise about future builds: when
Funcom ships a new one, the catalogue must be re-collected and the numbers will
differ.

The remaining ~11% are emitted as untyped generic templates deliberately. They
are types that own engine memory (`TArray<FString>`, `TMap`, multicast
delegates); passing them across the ABI by value would duplicate pointers and
risk a double free. A generic template that fails to compile is better than a
signature that corrupts memory silently.

---

## The ItemTable, and why Template ID is the identifier

Item identity in Conan Exiles does **not** work the way it does in ARK, and this
is the single most consequential difference for anyone porting a plugin.

The authoritative source is the DataTable at `/Game/Items/ItemTable`. Each row's
**Row Name is the Template ID**, and that is what identifies an item.

The row's `ItemClass` field points at a blueprint, but **it isn't unique**:

| item | Template ID | ItemClass |
|---|---|---|
| Stone | 10001 | `/Script/ConanSandbox.GameItem` |
| Brimstone | 14171 | `/Script/ConanSandbox.GameItem` — **the same** |
| Katana | 51091 | `/Game/Items/Weapons/Katana2h/BP_Item_KatanaBase…` |

Hundreds of simple items share that one native class. A shop modelled the ARK
way — keyed on blueprint path — would deliver the *wrong item* for that entire
family, and it would do so **without any error**, because the class exists and
the delivery succeeds.

The `ExtratorItemTable` plugin in this repository reads that DataTable directly
from the running server, through the engine's own
`GetDataTableRowNames` / `GetDataTableColumnAsString` — no struct layout is
assumed, so it doesn't break when row layout changes between builds.

On the tested build it extracts the complete ItemTable exposed by that build:
**9,121 rows × 120 columns**.

---

## Build compatibility

The runtime is validated against a specific game build and **refuses to load on
an unrecognised one**, deliberately, with the reason logged.

Validated build: **`24784646`** (Conan Exiles Enhanced, UE 5.6.1).

For plugin authors this matters in one specific case. A plugin that resolves
everything by name survives a game update, because names are stable where
addresses aren't. A plugin that **hardcodes raw offsets** doesn't, and the
failure is silent: it keeps running and reads the wrong memory.

If your plugin depends on raw offsets, declare it in your `PluginInfo.json`. The
loader will then refuse *your plugin* on an unvalidated build instead of letting
it run with wrong data. See
[If your plugin uses raw offsets, declare it](#if-your-plugin-uses-raw-offsets-declare-it).

---

## Security and trust model

A plugin built with this SDK is a native DLL running **inside the server
process, with that process's privileges**. There's no sandbox between plugins,
and there won't be one.

For you as an author, two consequences:

- **Your bugs crash the server.** A bad pointer in your plugin is a bad pointer
  in the dedicated server. The API contains what it can, load failures, most
  hook errors, and errors in scheduled work (that plugin is quarantined, the
  server continues) — but it can't contain memory corruption.
- **Publish your source.** Server administrators are installing a native binary
  that can read their players' identity data and any file the server can reach.
  Every example in this SDK ships with source, and that is the expectation for
  anything published on top of it. See [PUBLICAR-PLUGIN.md](PUBLICAR-PLUGIN.md).

---

## What is open, and what isn't

This project draws a deliberate line, and it's worth stating plainly so nobody
has to guess.

**Open source, the public plugin interface and reference implementations**

| | licence | where |
|---|---|---|
| the plugin ABI and every public header | MIT | [SDK](https://github.com/andrew-mauricio/Conan-Api-SDK) `include/Conan/` |
| the complete list of functions the API exposes to plugins | MIT | `ConanPluginApi.h` |
| plugin examples, with source | MIT | SDK `Exemplos/` |
| the Permission plugin, with source | MIT | SDK `Exemplos/Permission/` |
| a complete real plugin, with source and tests | MIT | [Conan-Shop](https://github.com/andrew-mauricio/Conan-Shop) |

**Distributed as a binary, the runtime**

The Conan-Api runtime and loader (`winmm.dll` and the packaged runtime) ship
compiled. Their source isn't published, and the licence is proprietary: you may
run them on as many servers as you like, including servers that charge players,
but you may not resell, re-host or redistribute them.

Documented in the open, even though the source isn't: how the loader enters the
process, what it does at startup and shutdown, the build check, the trust model,
and the full list of functions it exposes to plugins, all in this README and in
the public headers. Every release publishes the SHA-256 of its artifacts, so you
can verify that the file you downloaded is the file that was published.

> To be exact about what that does and doesn't give you: the SHA-256 lets you
> confirm the download wasn't tampered with in transit or re-hosted. It does
> **not** let you reproduce the runtime binary from published source, because
> that source isn't published. Where a reproducible build *is* claimed in this
> project, it's claimed for plugins whose source is public — Conan-Shop, for
> instance, where you can compile and compare the hash yourself.

**Private, the engineering that produced the API**

The tooling that discovers engine structures, resolves them without debug
symbols, generates the typed SDK, and adapts the runtime to a new game build is
maintained privately. That's the work that took the longest, and it's what
distinguishes this project.

None of it ships, none of it executes on your machine, and none of it's part of
any release.

---

## Building

### Visual Studio

1. **New Project** → *Dynamic-Link Library (DLL)*
2. **C/C++ → General → Additional Include Directories**: point at `include`
3. **C/C++ → Code Generation → Runtime Library**: `/MT`, not `/MD`
4. **Platform: x64**

Or straight from the command line:

```bat
cl /nologo /std:c++17 /O2 /EHsc /LD /MT ^
   /I "path\to\sdk\include" ^
   MyPlugin.cpp /Fe:MyPlugin.dll
```

**`/MT` really matters.** With `/MD`, your DLL depends on the Microsoft runtime
being installed on the machine, and many servers run under Wine, in a container
where that runtime may not exist. The symptom is `LoadLibrary` failing with a
generic code that explains nothing. With `/MT` the runtime goes inside your DLL
and the problem doesn't exist.

To check it worked:

```bat
dumpbin /dependents MyPlugin.dll
```

Only `KERNEL32.dll` should show up. If `MSVCP140.dll` or `VCRUNTIME140.dll`
appear, `/MT` did not take.

### mingw-w64

```bash
x86_64-w64-mingw32-g++ -std=c++17 -O2 -shared \
    -I path/to/include \
    -o MyPlugin.dll MyPlugin.cpp \
    -static-libgcc -static-libstdc++
```

The `-static-*` for the same reason as `/MT`.

### What we test on every version

| you use | works? | tested by us |
|---|---|---|
| Visual Studio 2026 (`cl` 19.51) | yes | **yes** — every release |
| Visual Studio 2017–2022 (v141–v143) | yes | not directly; the C ABI has not changed |
| mingw-w64 (GCC 13+) | yes | **yes** — every build |
| clang targeting Windows | yes | not directly |

This isn't a statement of intent. On every version we download the published
package, build `ExemploOla` with MinGW **and** with MSVC, and bring both
binaries up on the same server. If either one stops working, the release doesn't
ship.

---

## Start by copying an example

```bash
cp -r Exemplos/ExemploOla MyPlugin
cd MyPlugin
mv ExemploOla.cpp MyPlugin.cpp
sed -i 's/ExemploOla/MyPlugin/g' compilar.sh
./compilar.sh
```

| example | what it teaches |
|---|---|
| **ExemploOla** | the smallest plugin that still proves something: find an object, call a function, write to the log |
| **ExemploComando** | intercept `!command` in chat and swallow the message |
| **ExemploVigia** | welcome on login, count who is online, answer chat commands |
| **ExemploVip** | query Permission and keep working when it is not installed |
| **ExemploAgendado** | run code periodically, on the right thread |
| **ExemploBlueprint** | intercept Blueprint execution — what a hook by name cannot see. **Read the warning at the top of the file**: an earlier version of it left a window where every Blueprint execution was silently dropped, and since Conan's login is made of Blueprint, nobody could join the server |
| **Permission** | the complete plugin: database, configuration, and an ABI others consume |

---

## From chat to the player: the path every plugin needs

This is the jump missing from almost every API, and without it the 9,247 classes
are useless: **someone typed something, who was it, and where are they?**

```cpp
extern "C" ConanAcao AoFalar(ConanChamada* c)
{
    // 1. WHO: in a hook, c->Obj is the object that received the call.
    //    In chat, that is the ConanPlayerController of whoever typed.
    void* controller = c->Obj;

    // 2. THE NAME lives in the PlayerState, not in the controller.
    char nome[128] = "";
    if (void* ps = MembroPonteiro(controller, "PlayerState"))
    {
        const int32_t off = g_api->OffsetDoMembro(ps, "PlayerNamePrivate");
        if (off >= 0) g_api->LerTextoDoJogo(ps, uint32_t(off), nome, sizeof(nome));
    }

    // 3. THE CHARACTER. "Character" is the typed pawn; "Pawn" covers the rest.
    void* corpo = MembroPonteiro(controller, "Character");
    if (!corpo) corpo = MembroPonteiro(controller, "Pawn");

    // 4. THE POSITION, through the game's function — not the field, which is
    //    replicated.
    struct { double X, Y, Z; } pos{};
    g_api->ChamarFuncao(corpo, "K2_GetActorLocation", nullptr, nullptr, 0,
                        &pos, sizeof(pos));

    g_api->MensagemParaJogador(nome, "found you");
    return CONAN_CANCELAR;
}
```

`MembroPonteiro` is the helper that repeats in every plugin, it resolves the
offset **by name**, reads the pointer and refuses anything unreadable:

```cpp
static void* MembroPonteiro(void* obj, const char* nome)
{
    if (!obj) return nullptr;
    const int32_t off = g_api->OffsetDoMembro(obj, nome);   // through reflection
    if (off < 0) return nullptr;                            // not here

    void* p = nullptr;
    if (g_api->LerMembro(obj, uint32_t(off), &p, sizeof(p)) <= 0) return nullptr;
    if (!p || !g_api->Legivel(p, 8)) return nullptr;
    return p;
}
```

**No baked offsets.** Running on this server, `OffsetDoMembro` returned
`PlayerState → 0x308`, `Character → 0x350`, `Pawn → 0x340`. Writing those numbers
into your code works today and reads the neighbouring field after the next patch
— no error, no log, just wrong data.

### And with no hook at all?

When the starting point isn't the player talking, a scheduled task, an admin
command, the way in is to sweep:

```cpp
void* pcs[64];
int n = g_api->FindObjects("ConanPlayerController", pcs, 64, /*includeChildren=*/1);
```

From there it's the same: `PlayerState` for the name, `Character` for the body.

**Do not keep those pointers between calls.** The game's garbage collector
destroys objects and reuses addresses; `Legivel` would still say yes, because the
page stays mapped, and you would act on something else. Fetch them again on every
use.

The complete example, with logging and each case handled, is in
`Exemplos/ExemploJogador`.

---

## The structure of a plugin

```
Conan-Api/Plugins/MyPlugin/
   MyPlugin.dll         <- the loader looks for this name first
   PluginInfo.json      <- name, version, what you require (optional, but do it)
   config.json          <- your configuration
   mydatabase.db        <- whatever you write is born here
```

### What is required, and what isn't

**Only the DLL.** A plugin with nothing but that loads and works, this project's
`Cartografo` and `GravadorDeEventos` run exactly like that, and the log shows:

```
  [ok] Cartografo   [sem PluginInfo.json]
```

The other two files are your choices:

| file | required? | who reads it |
|---|---|---|
| `MyPlugin.dll` | **yes** — the only thing the loader needs | the loader |
| `PluginInfo.json` | no | the loader, **if** it exists |
| `config.json` | no | **your plugin**; the API never opens it |

`config.json` has no format imposed by anyone. The API only tells you **where**
it lives, through `CaminhoConfig("MyPlugin")`, and the rest is yours, it can be
JSON, it can have another name, it can not exist at all.

**Without `PluginInfo.json` you lose four things**, worth knowing before you
decide to skip it:

- your plugin's name and version in the log (only the folder name shows up)
- `MinApiVersion` — the loader can't refuse your plugin on an old API
- `Dependencies` — nothing guarantees Permission comes up before you
- `BuildDoJogo` / `UsaOffsetsCrus` — without them your plugin loads after a game
  update even when it should not

For a plugin you only use yourself, none of that matters. For one you
**publish**, all of it does.

`PluginInfo.json` is your plugin's identity card:

```json
{
  "FullName":      "My Plugin",
  "Description":   "What it does, in one line",
  "Version":       "1.0.0",
  "MinApiVersion": 3,
  "Dependencies":  ["Permission"]
}
```

**`MinApiVersion`** makes the loader **refuse** your plugin on an API that is too
old, instead of letting it run and read garbage. The table in this version is
**v6** — but declare the lowest number you actually need, not the highest. Asking
for v6 without using anything from v6 refuses to run on a server still on v5, for
no reason at all.

**`Dependencies`** makes sure Permission comes up before you. Without it you
would ask it before it exists and conclude it isn't installed. (That happened
here, with one of our own plugins.)

**Store everything through the API**, never by relative path:

```cpp
const char* db = g_api->CaminhoDados("MyPlugin", "mydatabase.db");
```

A relative path resolves from the **server's** directory, not your folder. One of
our plugins wrote 9 MB per boot in the wrong place that way.

---

## If your plugin uses raw offsets, declare it

This is the difference between a plugin that survives a Conan update and one that
starts reading the wrong memory in silence:

```json
{ "BuildDoJogo": 24784646, "UsaOffsetsCrus": true }
```

When you declare it and the game build changes, **the loader refuses your
plugin** and tells the server owner to ask you for a new version. Without the
declaration it loads and reads the neighbouring field, no error, no log, no
clue.

**How to not need this:** use `api->OffsetDoMembro(obj, "FieldName")` instead of
the number. It resolves through reflection, on whatever build is running, and
your plugin crosses the update without you doing anything.

---

## Answering in the first second

The server accepts players **before** the world finishes building. In that window
reflection doesn't exist yet, so a plugin switched on only after it leaves
whoever typed an early command with no answer.

There is a second, optional entry point for that:

```cpp
extern "C" __declspec(dllexport)
void ConanPluginRegistrar(const ConanApiTabela* api)   // BEFORE the world
{
    ConanApi::UsarTabela(api);
    g_id = api->HookProcessEvent("ServerSendChatMessage", AoFalar, nullptr, 100);
}
```

`HookProcessEvent` called here **goes into a queue** and returns a valid id right
away. The API arms the hook the moment the world builds, before any plugin is
switched on.

**What you can't do here:** touch a game object. There's no world, and
`FindClass`/`FindObjects` return nothing. If you need the world, the place is
`ConanPluginCarregar`.

You don't have to export `Registrar`. Without it everything works as before, it
only shortens the window.

---

## Using Permission

Nothing to link, it's `GetProcAddress` underneath, in a small header:

```cpp
#include "Conan/ConanPermission.h"

char id[64];
if (ConanPermIdDoController(controller, id, sizeof(id)) > 0)
    if (ConanPermTem(id, "myplugin.daily.kit", /*if_absent=*/0) == 1)
        GiveKit(controller);
```

If Permission isn't installed, the functions return the `if_absent` value you
passed and your plugin keeps running.

**Ask at the moment of use, not at load.** And pick `if_absent` by the cost of
being wrong: for a VIP kit, `0` (denying for thirty seconds is annoying; handing
it to everyone during a database outage cannot be undone).

---

## What the API won't let you do, and why

**Hook any address.** `HookFuncao` refuses about 32% of them, with the reason in
`TextoRecusa`. That isn't a failure: it's the API refusing to install a detour
that would one day execute half an instruction, corrupting memory hours later,
somewhere unrelated to the cause.

**Pass the wrong size.** `ChamarFuncao` checks the size of each argument against
the real parameter and refuses when they don't match. If you pass a `float`
where the game expects a `double`, it stops and says so. We measured: **293
functions** in this build corrupt the stack down that path, and the symptom shows
up far from the cause.

**Build a game string yourself.** The game destroys the parameter block on return
and calls **its own** allocator on whatever pointer is there. If that is your
memory, the server goes down, tested, not theory.

You don't need to do that: **write ordinary text and the API builds it**.

```cpp
g_api->MensagemParaTodos("The server restarts in 5 minutes.");
g_api->MensagemParaJogador("PlayerName", "Kit delivered. Come back in 24h.");
g_api->MensagemNaTela(playerController, "Welcome!", 8.0f);
```

Underneath, the API asks the **game itself** for the `FString` (or the `FText`)
and hands back what it built. None of your memory crosses the boundary, and
whoever allocates is whoever frees.

---

## When it doesn't work: where to look

Two files answer almost everything, in `Conan-Api/Logs/`:

| file | what it tells you |
|---|---|
| `ConanLoader.log` | which plugins the loader **saw**, which it **refused** and why |
| `ConanApi.log` | what plugins wrote with `Log()`, and the engine's warnings |

**The DLL won't open.** Almost always architecture (built x86 instead of x64)
or `/MD` instead of `/MT`. Windows error `193` means, in practice, 32-bit.

**It opened, but nothing happens.** Check that the folder name and the DLL name
match, and that you exported `ConanPluginCarregar`. On MSVC, without `extern "C"`
the name comes out decorated and the loader can't find it:

```bat
dumpbin /exports MyPlugin.dll | findstr ConanPlugin
```

You should see `ConanPluginCarregar`, not `?ConanPluginCarregar@@YAXPEBU...`.

**`ConanSDK.h` calls do nothing.** `ConanApi::UsarTabela(api)` is missing.

**The function returns `false` and you can't tell whether it ran.** Use
`api->UltimaChamadaExecutou()`. The game filters calls on template objects and
uninitialised actors, and in those cases the return comes from a zeroed block —
without that signal, "the function said no" and "the function did not run" become
the same `false`.

**You wrote to a field and the client doesn't see it.** The field is replicated;
there are 1,222 of the 36,210 in this build. Ask first with
`api->EhReplicado(...)` and prefer calling the game's own function, which walks
the path that already replicates.

---

![The Exiled Lands](.github/imagens/conan-3.jpg)

## Published a plugin?

Open an *issue* here and tell us. The idea is to keep a list in the README so
server owners can find what exists.

Before publishing, a short checklist:

- [ ] builds x64, with `/MT` (MSVC) or `-static-*` (MinGW)
- [ ] the folder is named after the plugin, and so is the DLL
- [ ] everything it writes goes through `CaminhoDados("YourPlugin", ...)`
- [ ] it checks `api->tamanho` before using the table
- [ ] `DllMain` does nothing
- [ ] if it uses Permission, it asks at use time and degrades when it's missing
- [ ] it ran on a real server, a test doesn't prove the real path

---

## To run a server

That's another repository: **[Conan-Api](https://github.com/andrew-mauricio/Conan-Api)**. The loader, the
ready-made package and the install guide live there.

They are separate on purpose: someone running a server needs no compiler at all,
and someone writing a plugin needs none of the server binaries.

---

## Licence

The SDK in this repository — headers, examples and tooling — is **MIT**. Write
plugins, modify the examples, sell what you build. Nothing of ours ends up
linked into your binary anyway: the plugin talks to a C function table at
runtime.

The **[Conan-Api runtime](https://github.com/andrew-mauricio/Conan-Api)** (the
loader that gets into the server process) has its own, more restrictive licence:
run it on as many servers as you like, including servers that charge players,
but don't resell or re-host the API itself.

Full text in [LICENSE](LICENSE) and [NOTICE.md](NOTICE.md).

---

## The three repositories

| repository | for whom |
|---|---|
| **[Conan-Api](https://github.com/andrew-mauricio/Conan-Api)** | server administrators — the loader and the packaged runtime |
| **[Conan-Api-SDK](https://github.com/andrew-mauricio/Conan-Api-SDK)** | plugin developers — headers, examples, reflected catalogue |
| **[Conan-Shop](https://github.com/andrew-mauricio/Conan-Shop)** | a finished shop plugin, and the reference implementation of a real plugin |

---

## Legal notice and attribution

**Conan-Api is an independent, community-developed project. It isn't affiliated
with, endorsed by, sponsored by, or supported by Funcom or Inflexion Games.**

*Conan Exiles* and all related marks are the property of Funcom. Promotional
images in this repository are official Steam material and remain the property of
their respective owners; they are used for identification only.

The reflected catalogue shipped here was produced by reading the publicly
distributed dedicated server's own reflection data, for the purpose of
interoperability, enabling administrators to extend servers they operate
themselves.

<p align="center">
  <a href="README.md"><img src=".github/imagens/bandeiras/us.png" alt="English" height="13">&nbsp;<b>English</b></a>
  &nbsp;&nbsp;&middot;&nbsp;&nbsp;
  <a href="Docs/README.pt.md"><img src=".github/imagens/bandeiras/br.png" alt="Portugu&ecirc;s" height="13">&nbsp;Portugu&ecirc;s</a>
  &nbsp;&nbsp;&middot;&nbsp;&nbsp;
  <a href="Docs/README.es.md"><img src=".github/imagens/bandeiras/es.png" alt="Espa&ntilde;ol" height="13">&nbsp;Espa&ntilde;ol</a>
</p>
