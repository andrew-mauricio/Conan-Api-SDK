// Comandos — Permission's chat commands (`!perm ...`).
//
// WHY THIS SPENT SO LONG AS NOTHING BUT PROSE
// -------------------------------------------
// This plugin's README has specified these commands from the start, with the
// minimum set, the chat struct's layout and even the reply path. And it ended
// by saying: *"the chat command is fully specified and only waits for the
// hook"*.
//
// It waited. Meanwhile, giving somebody VIP meant editing the database by hand
// (with WAL and an in-memory cache — asking for corruption) or writing a plugin
// just to call `conceder`. Andrew raised it on 2026-08-20, and he was right
// about the gap; only the cause was different from the one assumed.
//
// THE CAUSE WASN'T "RELOAD IS MISSING"
// ------------------------------------
// Granting and revoking ALWAYS took effect immediately: the writer thread ends
// with `if (ok && mexeu) Reconstruir();` (Armazem.cpp), which republishes the
// in-memory snapshot. Restarting for a VIP change was never necessary.
//
// What was missing was the DOOR: the function existed in the ABI and no human
// had any way to reach it. That's the gap this file closes.
//
// THE PREFIX IS `!`, NOT `/`
// --------------------------
// `/` belongs to the game (`/help`, and the chat itself treats it as its own
// command). `!` is what the community uses and what the rest of this house
// already follows (`!shop`, `!pontos`). Andrew corrected this before the first
// line was written.
#pragma once

#include "Conan/ConanPluginApi.h"

namespace Perm
{
    // Hooks chat. Returns false if the hook couldn't be registered — and then
    // the plugin carries on working, just without the commands: Permission is a
    // service to other plugins before it's an interface to anyone.
    bool LigarComandos(const ConanApiTabela* api);
    void DesligarComandos();
}
