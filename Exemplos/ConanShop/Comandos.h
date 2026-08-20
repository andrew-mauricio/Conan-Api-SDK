// Comandos (commands) — administering the shop FROM OUTSIDE THE GAME.
//
// WHY THIS ISN'T AN RCON COMMAND
// -------------------------------
// Because it can't be, and that was MEASURED on 2026-08-20, not assumed:
//
//   · Conan's RCON accepts a fixed list (`help` shows: listplayers, broadcast,
//     con, exec, sql, KickPlayer...). There's no registration of new commands —
//     AsaApi has AddRconCommand, Conan exposes no equivalent;
//   · the next hope was riding along: `broadcast <text>` calls
//     ConanCheatManager::BroadcastMessage, which IS a UFunction and therefore
//     hookable. It was tested with the ProvaDeRcon plugin, with all five
//     candidates hooked at once. The server answered "Message has been
//     broadcast." and NONE of the hooks fired: the RCON path is native and
//     doesn't go through reflection;
//   · and `exec` with no argument crashes the whole server (access violation).
//
// So the outside door is a FILE, which is what every automation already knows
// how to write: shell script, web panel, scheduled task, FTP, SSH.
//
// HOW IT WORKS
// ------------
// The owner writes lines into  Conan-Api/SHOP-COMANDOS  and the plugin, on its
// next wake-up (every 3 s), executes them and ANSWERS in
// Conan-Api/SHOP-RESPOSTAS.
//
//     dar        <player> <amount> [reason]      give points
//     tirar      <player> <amount> [reason]      take points
//     definir    <player> <amount> [reason]      set points
//     saldo      <player>                        balance
//     grupo      <player> <group> [days]         VIP, through Permission
//     tirargrupo <player> <group>                revoke
//     recarregar                                 reload the config
//
// `grupo` and `tirargrupo` call Permission, and work for ANY group in
// permission.json, not only the ones that grant points. They exist here because
// Permission has no outside door: anyone wanting to grant VIP by script would
// need a plugin just to call its ABI, or would have to edit the database by
// hand (with WAL and an in-memory cache, that's asking for corruption). The
// optional `days` is how VIP is actually sold: `grupo Someone#1 vip 30`.
//
// <player> may be the account id (what Permission uses) or the display name of
// someone online, resolved on the spot. The name only works while the player is
// connected, and the answer says so rather than failing quietly.
//
// THE TWO THINGS THIS DESIGN PROTECTS
// ------------------------------------
// 1. The file is DELETED before executing. If the server dies midway, the
//    command doesn't run twice on the way back — giving points twice is worse
//    than not giving them, because nobody complains and the owner never finds
//    out.
// 2. Every line produces a reply line, including the ones that failed. A silent
//    queue is a queue where the owner doesn't know whether it worked.
#pragma once

#include <string>

namespace Shop
{
    // Called by the scheduler. Reads the queue, executes, writes the replies.
    // Does nothing (and costs nothing) when the file isn't there.
    void AtenderFila();

    // The paths of both files, so the log can say where they are.
    const std::string& CaminhoDaFila();
    const std::string& CaminhoDasRespostas();
    void DefinirCaminhos(const std::string& raiz);
}
