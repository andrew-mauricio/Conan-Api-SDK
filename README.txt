Conan-Api SDK 2.7.0 — writing plugins for Conan Exiles
=========================================================

API table: v6
Targets game build 24784646 (UE 5.6.1 (++exiles+release))

(Este documento em portugues: LEIA-ME.txt)

WHAT YOU NEED
-------------
  A C++ compiler that produces a 64-bit Windows DLL. Nothing else.
  There's no library to link against, and no code of ours to build alongside.

  Visual Studio 2017, 2019 or 2022  (v141, v142, v143 — any of them)
     Project: Dynamic-Link Library (DLL)
     Platform: x64            <- required; a 32-bit DLL will not load
     C/C++ > General > Additional Include Directories: the include folder here
     C/C++ > Code Generation > Runtime Library: /MT   (NOT /MD)

  or mingw-w64 (Linux, macOS or Windows)
     x86_64-w64-mingw32-g++ -std=c++17 -O2 -shared \
         -I include -o MyPlugin.dll MyPlugin.cpp \
         -static-libgcc -static-libstdc++

  WHY /MT AND -static-*: without them your DLL depends on the compiler's
  runtime being installed on the machine. The server runs under Wine, in a
  container where it may not be — and the symptom is LoadLibrary failing with
  a generic code that explains nothing.

  WHY ANY COMPILER WORKS: what crosses the boundary is a plain-C struct of
  function pointers. No std::string of ours, no vtable of ours goes across, so
  none of the ABI grief that forces other APIs to demand their exact compiler.

WHAT'S IN HERE
--------------
  include/Conan/ConanPluginApi.h    the API table — this is all you include
  include/Conan/ConanPermission.h   look up VIP and permissions (optional)
  Exemplos/                         worked examples with source, to copy
  Docs/DEVELOPERS.md                the full guide
  Docs/EVENTS.md                    which game events exist, and their layout
  PUBLISHING-A-PLUGIN.md            what we ask of published plugins, and why

  Portuguese translations sit beside them as .pt.md.

START HERE
----------
  cp -r Exemplos/ExemploOla MyPlugin
  cd MyPlugin && ./compilar.sh

  Then copy the MyPlugin FOLDER (with the .dll inside) into the server's
  Conan-Api/Plugins/ and restart.

VERSIONS: WHAT HAS TO MATCH
---------------------------
  Your compiler does NOT have to match ours.
  Your DLL does have to be x64 and free of external runtime deps (/MT).

  In your PluginInfo.json, declare which table version you need:

      { "FullName":"My Plugin", "Version":"1.0.0", "MinApiVersion":6 }

  If the server has an older API than that, the loader REFUSES your plugin and
  says which version is missing, instead of letting it run against a struct
  that changed size.

  And always check this, on the first line of your ConanPluginCarregar:

      if (!api || api->tamanho < sizeof(ConanApiTabela)) return;

WHEN THE GAME UPDATES
---------------------
  What needs regenerating is the API, not your plugin: you talk to the table,
  and the table keeps its shape. The exception is a plugin using raw game
  offsets (the chat layout, say) — that one needs checking.

To RUN a server, grab the other package:
github.com/andrew-mauricio/Conan-Api
