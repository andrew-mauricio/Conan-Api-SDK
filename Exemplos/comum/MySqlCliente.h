// ============================================================================
//  MySqlCliente.h — MySQL client speaking the protocol straight on the socket
//
//  WHY THIS FILE EXISTS
//  --------------------
//  Permission keeps everything in a local SQLite, and that's still the
//  default. A server owner who WANTS to can point at a MySQL in config.json,
//  and for that they'd need a MySQL client. There's no libmysqlclient for
//  this toolchain (mingw-w64), and telling the owner to install a DLL is
//  exactly the friction this project exists to avoid: whoever runs a Conan
//  server isn't a programmer.
//
//  So the client is written here, the same way SQLite already comes embedded
//  (terceiros/sqlite3/, 261,454 lines inside our own .dll). The MySQL
//  client-server protocol is public and stable since version 4.1.
//
//  ZERO EXTERNAL DEPENDENCIES, FOR REAL
//  Winsock on Windows, BSD sockets on Linux, and nothing else. SHA-1,
//  SHA-256, RSA and OAEP are implemented in this .cpp because MySQL 8 auth
//  needs all four and there's no OpenSSL in this toolchain. No third-party
//  .lib, no vcpkg, no package for the owner to install.
//
//  ┌──────────────────────────────────────────────────────────────────────┐
//  │  THE RULE THAT GOVERNS EVERYTHING IN THIS FILE                       │
//  │                                                                      │
//  │  THIS BLOCKS. Conectar waits on the network. Executar waits for the  │
//  │  answer. NOTHING HERE CAN BE CALLED ON THE GAME THREAD — never,      │
//  │  under any circumstance, not even "just a quick little query".       │
//  │                                                                      │
//  │  A MySQL on the other side of the country, behind a bad network, is  │
//  │  the database's problem. If that wait happens in the game loop it    │
//  │  becomes the GAME SERVER's problem: the tick stops, players time     │
//  │  out and drop. Armazem already has the right writer thread for       │
//  │  this (Armazem.h: "WRITING happens outside the loop, on a            │
//  │  thread of its own").                                                │
//  │                                                                      │
//  │  The timeouts below limit the damage, they don't remove it: 10 s of  │
//  │  freeze in the game loop drops the server all the same.              │
//  └──────────────────────────────────────────────────────────────────────┘
//
//  INVARIANTS THIS FILE COMMITS TO KEEPING
//  ---------------------------------------
//  INV-MYSQL-001 · No database failure may become a process failure.
//      Any error — network, protocol, auth, hostile server, truncated
//      answer, giant packet — comes out as `false` + text in Portuguese.
//      Never as an exception crossing the boundary, never as a read past
//      the end of a buffer, never as an endless wait.
//
//  INV-MYSQL-002 · No call may wait forever.
//      Every read and every write has a deadline. Conectar has its own.
//      A server that accepts the connection and then goes quiet looks just
//      like a server that's up. Only the clock tells them apart.
//
//  INV-MYSQL-003 · The packet stream never goes out of sync in silence.
//      Sequence number checked on every packet. If it diverges the
//      connection is DROPPED, never reused. Reading on from a misaligned
//      stream hands back data from another query wearing the face of the
//      right data, and that's the bug nobody finds later.
//
//  INV-MYSQL-004 · Escapar() is only called where escaping actually works.
//      Two server conditions break byte-by-byte escaping, both in silence:
//      NO_BACKSLASH_ESCAPES in sql_mode, and a multibyte character set like
//      GBK/SJIS/BIG5. Conectar() forces utf8mb4 and refuses the connection
//      if it can't get out of NO_BACKSLASH_ESCAPES. Fails closed: no
//      connection beats a connection with broken escaping.
//
//  WHAT THIS FILE **DOESN'T** DO, better written down than discovered:
//    · it isn't thread-safe — one connection, one thread. There's a guard
//      that REFUSES concurrent use (see `m_emUso` in the .cpp), but it
//      catches the mistake, it doesn't fix it;
//    · it doesn't reconnect on its own. Once it drops, `Conectado()` starts
//      returning false and the layer above decides. A hidden reconnect
//      re-runs a command the server may already have applied;
//    · it doesn't speak TLS. The password is protected during auth (it
//      never goes in the clear on the wire), but the QUERIES travel open.
//      See "RESIDUAL RISK" at the top of the .cpp;
//    · it has no prepared statements. Interpolating with Citar()/Escapar()
//      is the way, and CLIENT_MULTI_STATEMENTS is off on purpose so that a
//      `'; DROP TABLE ...` has no way to become a second command.
// ============================================================================
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace Perm
{
    // Connection state. Declared here as a name only: the definition lives
    // in the .cpp because it needs <winsock2.h>, and dragging winsock2 into
    // every file that includes this header is a known recipe for cascading
    // errors (windows.h included before it redefines everything).
    struct MySqlInterno;

    class MySqlCliente
    {
    public:
        MySqlCliente();
        ~MySqlCliente();

        MySqlCliente(const MySqlCliente&)            = delete;
        MySqlCliente& operator=(const MySqlCliente&) = delete;

        // ── settings, all optional ──────────────────────────────────────────

        // Deadlines in milliseconds. Default: 5,000 to connect, 10,000 to
        // operate. Applies to the NEXT connection; changing it with a
        // connection already open doesn't touch the configured socket.
        //
        // A value <= 0 is refused (falls back to the default). "No limit"
        // isn't an option this file offers: see INV-MYSQL-002.
        void DefinirTempos(int msConectar, int msOperar);

        // Cap on the payload of ONE logical packet from the server. 64 MiB
        // by default.
        //
        // It's here because the MySQL header allows a chain of packets of
        // 16 MiB each with no declared end. Without a cap, a hostile server
        // (or a confused MySQL behind a proxy) makes this process allocate
        // until memory runs out, and what dies is the game server.
        void DefinirTetoResposta(size_t bytes);

        // Optional diagnostics. Gets ready-made lines in Portuguese. May be
        // null (the default). Called only on threads that call this class.
        void DefinirLog(void (*log)(void* ctx, const char* linha), void* ctx);

        // ── connection ──────────────────────────────────────────────────────

        // Returns false and fills 'erro' with READABLE text in Portuguese.
        //
        // 'banco' may be empty or null (connects without picking a database).
        // 'erro' may be null; 'tamErro' is the buffer size, and the message
        // always comes out '\0'-terminated (truncated if it doesn't fit).
        //
        // Calling this with a connection open disconnects the previous one.
        bool Conectar(const char* host, uint16_t porta, const char* usuario,
                      const char* senha, const char* banco, char* erro, int tamErro);

        void Desconectar();
        bool Conectado() const;

        // ── INTERROMPER: the one thing here another thread is allowed to call
        //
        // THE BUG THIS FIXES
        // Everything else here is "one connection, one thread". But there's a
        // moment when the other thread HAS to speak: game server shutdown.
        // Armazem::Fechar() tells the writer thread to leave and waits for it
        // (join). If it's sitting inside a recv() against a MySQL that
        // accepted the connection and went quiet (a firewall cutting it in
        // half, a stuck database), it only comes back when the deadline
        // blows: msOperar per command, and one rebuild is 7 queries, so up to
        // ~70 s of server "hung" on shutdown with the default deadlines.
        //
        // And then the owner does what anyone does: kills the process.
        // Killing the Conan server mid-shutdown means losing the world save.
        // A DATABASE problem turning into the worst damage possible in the
        // GAME.
        //
        // Interromper() does shutdown(SHUT_RDWR) on the socket: the call
        // blocked on the other thread returns right away, with an error, and
        // the operation fails like any other network failure
        // (INV-MYSQL-001). After that the connection refuses everything,
        // Conectar() included, until a new instance.
        //
        // WHY shutdown() AND NOT close(): close() frees the descriptor, and
        // the OS reuses the number a moment later. Closing here while the
        // other thread still holds the same number is the classic recipe for
        // sending the shutdown to a socket that became SOMETHING ELSE in the
        // meantime, and in this process those something elses are the GAME
        // SERVER's sockets. shutdown() doesn't free the descriptor; it stays
        // ours.
        void Interromper();

        // ── commands ────────────────────────────────────────────────────────

        // Runs SQL with no result set (INSERT/UPDATE/CREATE). false = error.
        //
        // If the SQL returns rows by mistake (a SELECT), they're READ and
        // thrown away. Leaving them unread puts the stream out of sync and
        // the next query reads this one's answer (INV-MYSQL-003).
        bool Executar(const char* sql, char* erro, int tamErro);

        // Query with a result set. Calls 'linha' for each record.
        //
        // 'ncols' is the column count; 'valores' has 'ncols' entries, and a
        // NULL value arrives as nullptr (an empty string arrives as "" —
        // they're different things and MySQL tells the two apart).
        //
        // Column names are in Colunas() and the value lengths for the current
        // row in ComprimentosDaLinha(), both of them valid DURING the 'linha'
        // call (vectors parallel to 'valores').
        //
        // 'linha' must not throw. If it does, it's caught here, the query is
        // aborted and the connection dropped (the rest of the result set
        // would sit on the wire, and reading half of it is the
        // INV-MYSQL-003 bug).
        bool Consultar(const char* sql,
                       void (*linha)(void* ctx, int ncols, const char* const* valores),
                       void* ctx, char* erro, int tamErro);

        // ── safe interpolation ──────────────────────────────────────────────

        // Escapes a string for interpolating into SQL. Does NOT put the
        // quotes around it, and whoever forgets the quotes is still
        // injectable. Prefer Citar().
        //
        // May throw std::bad_alloc (the class's only exception path; the
        // signature returns std::string and has nowhere to report an error).
        static std::string Escapar(const char* s);

        // The length-taking version: the only one that accepts a NUL in the
        // middle of the data. A bare `const char*` has no way to carry an
        // embedded 0x00, it ENDS there. Binary data has to come through here.
        static std::string Escapar(const char* s, size_t n);

        // Escapes AND wraps in single quotes: gives back  'texto'  ready for
        // the SQL. It's here because forgetting the quotes around Escapar()
        // is the easiest good-faith mistake to make and the most expensive:
        // escaping without quotes protects nothing.
        static std::string Citar(const char* s);
        static std::string Citar(const char* s, size_t n);

        // ── what's left over from the last command ──────────────────────────

        // After Executar: rows AFFECTED, as the server reported them.
        // After Consultar: rows READ. Different questions, and the same
        // counter answers both. It's written down here so nobody confuses
        // one with the other.
        uint64_t    LinhasAfetadas()   const;
        uint64_t    UltimoIdInserido() const;   // AUTO_INCREMENT of last INSERT
        unsigned    UltimoCodigoMysql()const;   // 0 = no error from the server
        const char* VersaoServidor()   const;   // raw string from the handshake

        const std::vector<std::string>& Colunas() const;
        const std::vector<size_t>&      ComprimentosDaLinha() const;

    private:
        MySqlInterno* m_i;   // opaque pointer; see the comment up top
    };
}
