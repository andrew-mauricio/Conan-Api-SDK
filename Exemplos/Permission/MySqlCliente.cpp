// ============================================================================
//  MySqlCliente.cpp — o protocolo MySQL, byte a byte, sem biblioteca nenhuma
//
//  COMO LER ESTE ARQUIVO
//  ---------------------
//    §1  utilidades: mensagem de erro, leitura com limite, número variável
//    §2  criptografia: SHA-1, SHA-256, aleatório do sistema
//    §3  RSA: número grande, PEM/DER, OAEP  (só o MySQL 8 precisa disto)
//    §4  socket com prazo (Winsock ou BSD)
//    §5  camada de pacote (cabeçalho de 4 bytes, número de sequência)
//    §6  handshake e autenticação
//    §7  comandos e leitura de resultado
//    §8  escape e aspas
//    §9  a classe pública
//
//  COMO LINKAR
//  -----------
//    Windows:  x86_64-w64-mingw32-g++ ... MySqlCliente.cpp -lws2_32
//    Linux:    g++ ... MySqlCliente.cpp        (nada a linkar)
//  Esquecer o -lws2_32 dá erro de link, não erro silencioso — é o tipo bom.
//
//  RISCO RESIDUAL, DECLARADO E NÃO RESOLVIDO AQUI
//  ----------------------------------------------
//  1) SEM TLS. A senha nunca vai em claro no fio (native = SHA-1 com sal;
//     caching_sha2 = SHA-256 com sal, ou RSA quando o servidor exige), mas as
//     CONSULTAS e as RESPOSTAS trafegam abertas. Quem consegue farejar a rede
//     entre o servidor de jogo e o MySQL lê e altera SQL. Para MySQL na mesma
//     máquina ou na mesma rede privada isso é aceitável; para MySQL na
//     internet, não é. Não está implementado e não é honesto dizer que está.
//
//  2) A chave RSA vem do PRÓPRIO servidor, pelo fio aberto (é o que o
//     protocolo caching_sha2_password oferece quando não há TLS). Quem
//     consegue se pôr no meio troca a chave pela dele e captura a senha. O
//     cliente oficial tem a mesma exposição quando roda com
//     --get-server-public-key; a alternativa (--server-public-key-path) exige
//     que o dono copie um arquivo .pem à mão, e a premissa deste projeto é
//     que ele não vai fazer isso. Fica escrito.
//
//  3) Sem prepared statements. A defesa contra injeção é Citar()/Escapar()
//     mais três coisas que este arquivo garante: utf8mb4 forçado,
//     NO_BACKSLASH_ESCAPES recusado, e CLIENT_MULTI_STATEMENTS desligado.
//
//  FONTE DO PROTOCOLO
//  ------------------
//  Documentação pública do MySQL ("Client/Server Protocol", capítulo do
//  MySQL Internals) e o comportamento observado nos dois servidores de teste
//  desta máquina: MySQL 8.4.11 e MariaDB 10.11. Cada campo lido está anotado
//  com o tamanho e a ordem de bytes — nada aqui faz cast de struct em cima do
//  buffer da rede, justamente para não depender de alinhamento nem de
//  endianness da máquina que compila (exigência 5 da tarefa).
// ============================================================================

#include "MySqlCliente.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <mutex>
#include <new>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  typedef SOCKET TSoq;
  #define SOQ_NULO      INVALID_SOCKET
  #define FECHAR_SOQ(s) closesocket(s)
  #define ERRO_SOQ()    WSAGetLastError()
  #define SOQ_BLOQUEIA  WSAEWOULDBLOCK
  #define SOQ_INTERROMP WSAEINTR
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <sys/select.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  #include <poll.h>
  typedef int TSoq;
  #define SOQ_NULO      (-1)
  #define FECHAR_SOQ(s) ::close(s)
  #define ERRO_SOQ()    errno
  #define SOQ_BLOQUEIA  EAGAIN
  #define SOQ_INTERROMP EINTR
#endif

namespace Perm
{
namespace
{

// ============================================================================
//  §1 · UTILIDADES
// ============================================================================

// Número grande em texto.
//
// POR QUE NÃO "%llu": o mingw-w64 pode ligar o printf do msvcrt, onde o
// modificador `ll` nem sempre existe; o resultado é um "%llu" impresso
// literalmente ou lixo — numa MENSAGEM DE ERRO, que é justamente a hora em
// que o dono do servidor mais precisa de um número legível. Com %s e esta
// função o texto é o mesmo em toda plataforma.
std::string Dec(uint64_t v)
{
    char b[24];
    int i = (int)sizeof(b);
    b[--i] = '\0';
    if (v == 0) b[--i] = '0';
    while (v) { b[--i] = (char)('0' + (int)(v % 10)); v /= 10; }
    return std::string(b + i);
}

// Escreve a mensagem no buffer do chamador. 'erro' pode ser nulo — é o caso
// comum de quem só quer saber se deu certo — e nesse caso não faz nada.
// Sempre termina em '\0'; se não couber, trunca (nunca estoura).
void Diga(char* erro, int tamErro, const char* fmt, ...)
{
    if (!erro || tamErro <= 0) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(erro, (size_t)tamErro, fmt, ap);
    va_end(ap);
    if (n < 0) erro[0] = '\0';           // vsnprintf falhou: não deixa lixo
    erro[tamErro - 1] = '\0';
}

// ── inteiros do protocolo ────────────────────────────────────────────────
//
// EXIGÊNCIA 5 DA TAREFA, e o motivo dela: o protocolo do MySQL é
// little-endian SEMPRE, independente da máquina. Ler com
// `*(uint32_t*)p` daria certo em x86 e daria errado em qualquer máquina
// big-endian — e daria errado também em x86 se o ponteiro estivesse
// desalinhado numa build com -fsanitize=alignment. As funções abaixo montam
// o número a partir dos bytes, na ordem que o protocolo manda, e por isso não
// dependem nem da endianness nem do alinhamento da máquina que compila.
inline uint16_t Le16(const uint8_t* p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
inline uint32_t Le24(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}
inline uint32_t Le32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline void PorLe32(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back((uint8_t)(x & 0xFF));
    v.push_back((uint8_t)((x >> 8) & 0xFF));
    v.push_back((uint8_t)((x >> 16) & 0xFF));
    v.push_back((uint8_t)((x >> 24) & 0xFF));
}

// ── cursor com limite ────────────────────────────────────────────────────
//
// TODA leitura de payload passa por aqui. O payload vem da rede: pode estar
// truncado por acidente (conexão caiu no meio) ou por maldade (servidor
// forjado dizendo "tenho 40 colunas" num pacote de 3 bytes).
//
// A regra é uma só: nenhuma função deste arquivo indexa o buffer da rede
// direto. Quem quer bytes pede ao cursor, e o cursor devolve false quando não
// tem. Um `false` ignorado não vira leitura fora do buffer porque o ponteiro
// devolvido continua nulo e o chamador confere.
class Cursor
{
public:
    Cursor(const uint8_t* p, size_t n) : m_p(p), m_n(n), m_i(0), m_ok(true) {}

    bool Ok()       const { return m_ok; }
    size_t Restam() const { return m_i <= m_n ? m_n - m_i : 0; }
    size_t Onde()   const { return m_i; }

    bool Bytes(size_t n, const uint8_t** saida)
    {
        if (!m_ok || Restam() < n) { m_ok = false; return false; }
        if (saida) *saida = m_p + m_i;
        m_i += n;
        return true;
    }
    bool U8(uint8_t& v)
    {
        const uint8_t* p = nullptr;
        if (!Bytes(1, &p)) return false;
        v = p[0];
        return true;
    }
    bool U16(uint16_t& v)
    {
        const uint8_t* p = nullptr;
        if (!Bytes(2, &p)) return false;
        v = Le16(p);
        return true;
    }
    bool U32(uint32_t& v)
    {
        const uint8_t* p = nullptr;
        if (!Bytes(4, &p)) return false;
        v = Le32(p);
        return true;
    }
    void Pular(size_t n) { Bytes(n, nullptr); }

    // Inteiro de tamanho variável ("length-encoded integer"):
    //   0x00..0xFA  o próprio byte é o valor
    //   0xFB        NULL (só faz sentido dentro de linha de resultado)
    //   0xFC        seguem 2 bytes little-endian
    //   0xFD        seguem 3 bytes little-endian
    //   0xFE        seguem 8 bytes little-endian
    //   0xFF        nunca aparece aqui — é o primeiro byte de um pacote ERR
    bool VarInt(uint64_t& v, bool& nulo)
    {
        nulo = false;
        uint8_t b = 0;
        if (!U8(b)) return false;
        if (b <= 0xFA) { v = b; return true; }
        if (b == 0xFB) { nulo = true; v = 0; return true; }
        const uint8_t* p = nullptr;
        size_t n = (b == 0xFC) ? 2 : (b == 0xFD) ? 3 : (b == 0xFE) ? 8 : 0;
        if (n == 0) { m_ok = false; return false; }   // 0xFF: fluxo inválido
        if (!Bytes(n, &p)) return false;
        v = 0;
        for (size_t i = 0; i < n; ++i)
            v |= (uint64_t)p[i] << (8 * i);           // little-endian, sempre
        return true;
    }

    // String precedida do comprimento variável. NULL vira nulo=true.
    bool VarStr(const uint8_t** p, size_t& n, bool& nulo)
    {
        uint64_t len = 0;
        if (!VarInt(len, nulo)) return false;
        if (nulo) { *p = nullptr; n = 0; return true; }
        if (len > Restam()) { m_ok = false; return false; }
        n = (size_t)len;
        return Bytes(n, p);
    }

    // String terminada em '\0'. Se não houver '\0' até o fim do payload, o
    // pacote está malformado — e aí a leitura FALHA, em vez de sair do buffer
    // procurando um zero que não existe.
    bool StrNul(std::string& saida)
    {
        if (!m_ok) return false;
        size_t j = m_i;
        while (j < m_n && m_p[j] != 0) ++j;
        if (j >= m_n) { m_ok = false; return false; }
        saida.assign((const char*)m_p + m_i, j - m_i);
        m_i = j + 1;
        return true;
    }

    // O que sobrou, cru.
    void Resto(const uint8_t** p, size_t& n)
    {
        *p = m_p + m_i;
        n  = Restam();
        m_i = m_n;
    }

private:
    const uint8_t* m_p;
    size_t         m_n;
    size_t         m_i;
    bool           m_ok;
};

// ============================================================================
//  §2 · CRIPTOGRAFIA MÍNIMA
//
//  SHA-1 e SHA-256 estão aqui porque a autenticação do MySQL usa os dois e
//  não há OpenSSL neste toolchain. São implementações diretas da FIPS 180-4,
//  sem otimização — rodam uma vez por conexão, não estão em caminho quente.
//  A prova de que estão certas está no teste (testes/teste_mysql.cpp): os
//  vetores conhecidos de "abc" e da string vazia, e — mais importante — o
//  fato de a autenticação REAL passar contra dois servidores diferentes. Um
//  SHA errado não autentica em lugar nenhum.
// ============================================================================

struct Sha1
{
    uint32_t h[5];
    uint64_t total;
    uint8_t  buf[64];
    size_t   n;

    void Iniciar()
    {
        h[0]=0x67452301u; h[1]=0xEFCDAB89u; h[2]=0x98BADCFEu;
        h[3]=0x10325476u; h[4]=0xC3D2E1F0u;
        total = 0; n = 0;
    }
    static uint32_t Rot(uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }

    void Bloco(const uint8_t* p)
    {
        uint32_t w[80];
        // big-endian: o SHA-1 é definido em big-endian, ao contrário do
        // protocolo do MySQL. Montado byte a byte pelo mesmo motivo de sempre.
        for (int i = 0; i < 16; ++i)
            w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16)
                 | ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
        for (int i = 16; i < 80; ++i)
            w[i] = Rot(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

        uint32_t a=h[0], b=h[1], c=h[2], d=h[3], e=h[4];
        for (int i = 0; i < 80; ++i)
        {
            uint32_t f, k;
            if      (i < 20) { f = (b & c) | (~b & d);            k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8F1BBCDCu; }
            else             { f = b ^ c ^ d;                     k = 0xCA62C1D6u; }
            uint32_t t = Rot(a,5) + f + e + k + w[i];
            e = d; d = c; c = Rot(b,30); b = a; a = t;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e;
    }
    void Adicionar(const uint8_t* p, size_t len)
    {
        total += (uint64_t)len * 8;
        while (len)
        {
            size_t cabe = 64 - n;
            size_t leva = len < cabe ? len : cabe;
            memcpy(buf + n, p, leva);
            n += leva; p += leva; len -= leva;
            if (n == 64) { Bloco(buf); n = 0; }
        }
    }
    void Fechar(uint8_t saida[20])
    {
        uint8_t fim = 0x80;
        uint64_t bits = total;
        Adicionar(&fim, 1);
        total = bits;                      // Adicionar somou o 0x80; desfaz
        static const uint8_t zero[64] = {0};
        while (n != 56) { Adicionar(zero, 1); total = bits; }
        uint8_t tam[8];
        for (int i = 0; i < 8; ++i) tam[i] = (uint8_t)((bits >> (56 - 8*i)) & 0xFF);
        Adicionar(tam, 8);
        for (int i = 0; i < 5; ++i)
        {
            saida[i*4  ] = (uint8_t)((h[i] >> 24) & 0xFF);
            saida[i*4+1] = (uint8_t)((h[i] >> 16) & 0xFF);
            saida[i*4+2] = (uint8_t)((h[i] >>  8) & 0xFF);
            saida[i*4+3] = (uint8_t)( h[i]        & 0xFF);
        }
    }
};

void Sha1De(const uint8_t* p, size_t n, uint8_t saida[20])
{
    Sha1 s; s.Iniciar(); s.Adicionar(p, n); s.Fechar(saida);
}
void Sha1De2(const uint8_t* a, size_t na, const uint8_t* b, size_t nb, uint8_t saida[20])
{
    Sha1 s; s.Iniciar(); s.Adicionar(a, na); s.Adicionar(b, nb); s.Fechar(saida);
}

struct Sha256
{
    uint32_t h[8];
    uint64_t total;
    uint8_t  buf[64];
    size_t   n;

    void Iniciar()
    {
        static const uint32_t iv[8] = {
            0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
            0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u };
        memcpy(h, iv, sizeof(h)); total = 0; n = 0;
    }
    static uint32_t Rot(uint32_t x, int s) { return (x >> s) | (x << (32 - s)); }

    void Bloco(const uint8_t* p)
    {
        static const uint32_t k[64] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,
        0x923f82a4u,0xab1c5ed5u,0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
        0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,0xe49b69c1u,0xefbe4786u,
        0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,
        0x06ca6351u,0x14292967u,0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
        0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,0xa2bfe8a1u,0xa81a664bu,
        0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,
        0x5b9cca4fu,0x682e6ff3u,0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
        0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u };

        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16)
                 | ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
        for (int i = 16; i < 64; ++i)
        {
            uint32_t s0 = Rot(w[i-15],7) ^ Rot(w[i-15],18) ^ (w[i-15] >> 3);
            uint32_t s1 = Rot(w[i-2],17) ^ Rot(w[i-2],19)  ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; ++i)
        {
            uint32_t S1 = Rot(e,6) ^ Rot(e,11) ^ Rot(e,25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + k[i] + w[i];
            uint32_t S0 = Rot(a,2) ^ Rot(a,13) ^ Rot(a,22);
            uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + mj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    void Adicionar(const uint8_t* p, size_t len)
    {
        total += (uint64_t)len * 8;
        while (len)
        {
            size_t cabe = 64 - n;
            size_t leva = len < cabe ? len : cabe;
            memcpy(buf + n, p, leva);
            n += leva; p += leva; len -= leva;
            if (n == 64) { Bloco(buf); n = 0; }
        }
    }
    void Fechar(uint8_t saida[32])
    {
        uint64_t bits = total;
        uint8_t fim = 0x80;
        Adicionar(&fim, 1); total = bits;
        static const uint8_t zero[1] = {0};
        while (n != 56) { Adicionar(zero, 1); total = bits; }
        uint8_t tam[8];
        for (int i = 0; i < 8; ++i) tam[i] = (uint8_t)((bits >> (56 - 8*i)) & 0xFF);
        Adicionar(tam, 8);
        for (int i = 0; i < 8; ++i)
        {
            saida[i*4  ] = (uint8_t)((h[i] >> 24) & 0xFF);
            saida[i*4+1] = (uint8_t)((h[i] >> 16) & 0xFF);
            saida[i*4+2] = (uint8_t)((h[i] >>  8) & 0xFF);
            saida[i*4+3] = (uint8_t)( h[i]        & 0xFF);
        }
    }
};

void Sha256De(const uint8_t* p, size_t n, uint8_t saida[32])
{
    Sha256 s; s.Iniciar(); s.Adicionar(p, n); s.Fechar(saida);
}
void Sha256De2(const uint8_t* a, size_t na, const uint8_t* b, size_t nb, uint8_t saida[32])
{
    Sha256 s; s.Iniciar(); s.Adicionar(a, na); s.Adicionar(b, nb); s.Fechar(saida);
}

// Apaga segredo da memória sem que o compilador otimize o memset embora.
// -O2 remove memset em buffer que "não é mais lido"; o ponteiro volátil
// impede isso. Não é paranoia gratuita: o buffer da senha e o do escrambleo
// ficam no stack de uma thread que continua viva depois.
void Limpar(void* p, size_t n)
{
    volatile uint8_t* v = (volatile uint8_t*)p;
    while (n--) *v++ = 0;
}

// ── aleatoriedade do sistema ─────────────────────────────────────────────
//
// Serve a UMA coisa: a semente do preenchimento OAEP que cifra a senha na
// autenticação do MySQL 8. Semente previsível enfraquece o OAEP e permite a
// quem gravou o tráfego testar palpites de senha fora do ar.
//
// Por isso: ou vem do gerador do sistema operacional, ou NÃO VAI. Não existe
// caminho "usa relógio e endereço de memória se não achar nada" — isso seria
// devolver criptografia de fachada com cara de criptografia.
// 'fonte' (opcional) recebe o nome do gerador que respondeu. Serve para o
// diagnostico na maquina do dono do servidor: se um dia a cifragem falhar, a
// primeira pergunta e' "qual gerador o Windows dele tem", e adivinhar isso a
// distancia custa uma tarde.
bool AleatorioDoSistema(uint8_t* p, size_t n, const char** fonte = nullptr)
{
    if (fonte) *fonte = "(nenhum)";
    if (n == 0) return true;
#ifdef _WIN32
    // Carregado em tempo de execução para NÃO acrescentar dependência de link
    // (nem -lbcrypt nem -ladvapi32). Se a DLL não existir, cai para a próxima.
    {
        typedef long (WINAPI *FnGen)(void*, unsigned char*, unsigned long, unsigned long);
        HMODULE m = LoadLibraryA("bcrypt.dll");
        if (m)
        {
            FnGen f = (FnGen)(void*)GetProcAddress(m, "BCryptGenRandom");
            // 0x00000002 = BCRYPT_USE_SYSTEM_PREFERRED_RNG (dispensa handle)
            if (f && f(nullptr, p, (unsigned long)n, 0x00000002u) == 0)
            { FreeLibrary(m); if (fonte) *fonte = "BCryptGenRandom"; return true; }
            FreeLibrary(m);
        }
    }
    {
        // RtlGenRandom: existe desde o Windows XP, e é o que o próprio
        // Windows usa por baixo. O nome exportado é SystemFunction036.
        typedef unsigned char (WINAPI *FnRtl)(void*, unsigned long);
        HMODULE m = LoadLibraryA("advapi32.dll");
        if (m)
        {
            FnRtl f = (FnRtl)(void*)GetProcAddress(m, "SystemFunction036");
            if (f && f(p, (unsigned long)n))
            { FreeLibrary(m); if (fonte) *fonte = "RtlGenRandom (advapi32)"; return true; }
            FreeLibrary(m);
        }
    }
    return false;
#else
    FILE* f = fopen("/dev/urandom", "rb");
    if (!f) return false;
    size_t lidos = fread(p, 1, n, f);
    fclose(f);
    if (lidos == n && fonte) *fonte = "/dev/urandom";
    return lidos == n;
#endif
}

// ============================================================================
//  §3 · RSA — só para o caching_sha2_password do MySQL 8
//
//  Números grandes em base 2^32, vetor little-endian (limbo 0 = menos
//  significativo). A redução é bit a bit: ~50 ms para uma exponenciação de
//  2048 bits com expoente 65537 (medido; ver saída do teste). É lento e é
//  óbvio — nesta ordem de importância, porque roda UMA vez por conexão e o
//  custo de um erro sutil aqui é uma autenticação que falha sem explicação.
// ============================================================================

typedef std::vector<uint32_t> Num;

void NumEnxugar(Num& a) { while (a.size() > 1 && a.back() == 0) a.pop_back(); }

Num NumDeBytes(const uint8_t* p, size_t n)          // entrada big-endian
{
    Num a;
    a.push_back(0);
    for (size_t i = 0; i < n; ++i)
    {
        // a = a*256 + p[i]
        uint32_t carry = p[i];
        for (size_t j = 0; j < a.size(); ++j)
        {
            uint64_t t = ((uint64_t)a[j] << 8) | carry;
            a[j] = (uint32_t)(t & 0xFFFFFFFFu);
            carry = (uint32_t)(t >> 32);
        }
        if (carry) a.push_back(carry);
    }
    NumEnxugar(a);
    return a;
}

// Saída big-endian de largura fixa. Devolve false se não couber — nunca
// trunca em silêncio, porque um RSA truncado "quase funciona" e falha só às
// vezes, que é o pior modo de falha possível.
bool NumParaBytes(const Num& a, uint8_t* saida, size_t n)
{
    memset(saida, 0, n);
    for (size_t j = 0; j < a.size(); ++j)
    {
        uint32_t v = a[j];
        for (int b = 0; b < 4; ++b)
        {
            size_t pos = j * 4 + (size_t)b;          // posição em bytes, do fim
            uint8_t byte = (uint8_t)((v >> (8 * b)) & 0xFF);
            if (pos >= n) { if (byte) return false; continue; }
            saida[n - 1 - pos] = byte;
        }
    }
    return true;
}

size_t NumBits(const Num& a)
{
    for (size_t i = a.size(); i-- > 0; )
        if (a[i])
        {
            size_t b = 0;
            uint32_t v = a[i];
            while (v) { ++b; v >>= 1; }
            return i * 32 + b;
        }
    return 0;
}
bool NumBit(const Num& a, size_t i)
{
    size_t l = i / 32;
    return l < a.size() && ((a[l] >> (i % 32)) & 1u) != 0;
}
int NumCmp(const Num& a, const Num& b)
{
    size_t na = a.size(), nb = b.size();
    while (na > 1 && a[na-1] == 0) --na;
    while (nb > 1 && b[nb-1] == 0) --nb;
    if (na != nb) return na < nb ? -1 : 1;
    for (size_t i = na; i-- > 0; )
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}
void NumSub(Num& a, const Num& b)                    // exige a >= b
{
    uint64_t emprestimo = 0;
    for (size_t i = 0; i < a.size(); ++i)
    {
        uint64_t bi = (i < b.size()) ? b[i] : 0;
        uint64_t t  = (uint64_t)a[i] - bi - emprestimo;
        a[i] = (uint32_t)(t & 0xFFFFFFFFu);
        emprestimo = (t >> 63) & 1u;                 // pediu emprestado?
    }
    NumEnxugar(a);
}
void NumShl1(Num& a)
{
    uint32_t carry = 0;
    for (size_t i = 0; i < a.size(); ++i)
    {
        uint32_t novo = a[i] >> 31;
        a[i] = (a[i] << 1) | carry;
        carry = novo;
    }
    if (carry) a.push_back(carry);
}
Num NumMul(const Num& a, const Num& b)
{
    Num r(a.size() + b.size(), 0);
    for (size_t i = 0; i < a.size(); ++i)
    {
        uint64_t carry = 0;
        for (size_t j = 0; j < b.size(); ++j)
        {
            uint64_t t = (uint64_t)a[i] * b[j] + r[i+j] + carry;
            r[i+j] = (uint32_t)(t & 0xFFFFFFFFu);
            carry  = t >> 32;
        }
        size_t k = i + b.size();
        while (carry) { uint64_t t = r[k] + carry; r[k] = (uint32_t)(t & 0xFFFFFFFFu); carry = t >> 32; ++k; }
    }
    NumEnxugar(r);
    return r;
}
Num NumMod(const Num& x, const Num& n)
{
    Num r; r.push_back(0);
    size_t bits = NumBits(x);
    for (size_t i = bits; i-- > 0; )
    {
        NumShl1(r);
        if (NumBit(x, i)) r[0] |= 1u;
        if (NumCmp(r, n) >= 0) NumSub(r, n);
    }
    NumEnxugar(r);
    return r;
}
Num NumModExp(const Num& base, const Num& e, const Num& n)
{
    Num b = NumMod(base, n);
    Num r; r.push_back(1);
    size_t bits = NumBits(e);
    for (size_t i = bits; i-- > 0; )
    {
        r = NumMod(NumMul(r, r), n);
        if (NumBit(e, i)) r = NumMod(NumMul(r, b), n);
    }
    return r;
}

// ── base64 e DER ─────────────────────────────────────────────────────────

int Base64Valor(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
bool Base64Decodificar(const char* p, size_t n, std::vector<uint8_t>& saida)
{
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; ++i)
    {
        char c = p[i];
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        if (c == '=') break;
        int v = Base64Valor(c);
        if (v < 0) return false;                       // lixo no meio: recusa
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            saida.push_back((uint8_t)((acc >> bits) & 0xFF));
        }
    }
    return !saida.empty();
}

// Andador de DER: só o suficiente para uma chave pública RSA. Cada leitura é
// conferida contra o fim do buffer (a chave vem da rede).
struct Der
{
    const uint8_t* p; size_t n; size_t i;
    Der(const uint8_t* pp, size_t nn) : p(pp), n(nn), i(0) {}
    bool Ler(uint8_t& tag, const uint8_t*& cont, size_t& tam)
    {
        if (i + 2 > n) return false;
        tag = p[i++];
        size_t len = p[i++];
        if (len & 0x80)
        {
            size_t nb = len & 0x7F;
            if (nb == 0 || nb > 4 || i + nb > n) return false;  // indefinido: não
            len = 0;
            for (size_t k = 0; k < nb; ++k) len = (len << 8) | p[i++];
        }
        if (len > n - i) return false;
        cont = p + i;
        tam  = len;
        i   += len;
        return true;
    }
};

// Extrai módulo e expoente de uma chave pública RSA em PEM.
// Aceita os dois formatos que os servidores MySQL/MariaDB emitem:
//   "BEGIN PUBLIC KEY"      → X.509 SubjectPublicKeyInfo (o que o MySQL manda)
//   "BEGIN RSA PUBLIC KEY"  → PKCS#1 puro
bool RsaDoPem(const char* pem, size_t pemN, Num& mod, Num& exp, size_t& kBytes,
              char* erro, int tamErro)
{
    std::string t(pem, pemN);
    size_t a = t.find("-----BEGIN");
    size_t b = t.find("-----END");
    if (a == std::string::npos || b == std::string::npos || b <= a)
    {
        Diga(erro, tamErro, "o MySQL mandou uma chave publica que nao esta no formato PEM "
                            "(nao achei as marcas BEGIN/END). Nao vou tentar adivinhar o "
                            "formato de uma chave: a conexao para aqui.");
        return false;
    }
    size_t inicioLinha = t.find('\n', a);
    if (inicioLinha == std::string::npos || inicioLinha >= b)
    {
        Diga(erro, tamErro, "o MySQL mandou uma chave publica em PEM truncada.");
        return false;
    }
    std::vector<uint8_t> der;
    if (!Base64Decodificar(t.c_str() + inicioLinha + 1, b - inicioLinha - 1, der))
    {
        Diga(erro, tamErro, "nao consegui decodificar (base64) a chave publica que o MySQL mandou.");
        return false;
    }

    Der d(der.data(), der.size());
    uint8_t tag = 0; const uint8_t* c = nullptr; size_t tam = 0;
    if (!d.Ler(tag, c, tam) || tag != 0x30)
    {
        Diga(erro, tamErro, "a chave publica do MySQL nao comeca com uma SEQUENCE DER.");
        return false;
    }

    Der dentro(c, tam);
    uint8_t t1 = 0; const uint8_t* c1 = nullptr; size_t n1 = 0;
    if (!dentro.Ler(t1, c1, n1))
    {
        Diga(erro, tamErro, "a chave publica do MySQL esta truncada (DER incompleto).");
        return false;
    }

    const uint8_t* pkcs1 = nullptr; size_t pkcs1N = 0;
    if (t1 == 0x30)
    {
        // SubjectPublicKeyInfo: SEQUENCE{ AlgorithmIdentifier, BIT STRING }.
        // Não conferimos o OID de propósito: se o servidor mandasse uma chave
        // que não é RSA, a leitura dos dois INTEGER abaixo falharia de
        // qualquer jeito, e falhar por não achar o número é tão fechado
        // quanto falhar por não achar o OID.
        uint8_t t2 = 0; const uint8_t* c2 = nullptr; size_t n2 = 0;
        if (!dentro.Ler(t2, c2, n2) || t2 != 0x03 || n2 < 2)
        {
            Diga(erro, tamErro, "a chave publica do MySQL nao tem o BIT STRING esperado.");
            return false;
        }
        pkcs1  = c2 + 1;      // primeiro byte do BIT STRING = bits não usados
        pkcs1N = n2 - 1;
        Der d2(pkcs1, pkcs1N);
        uint8_t t3 = 0; const uint8_t* c3 = nullptr; size_t n3 = 0;
        if (!d2.Ler(t3, c3, n3) || t3 != 0x30)
        {
            Diga(erro, tamErro, "a chave publica do MySQL tem BIT STRING sem SEQUENCE dentro.");
            return false;
        }
        pkcs1 = c3; pkcs1N = n3;
    }
    else if (t1 == 0x02)
    {
        // PKCS#1 direto: o primeiro elemento já é o INTEGER do módulo.
        pkcs1  = c;
        pkcs1N = tam;
    }
    else
    {
        Diga(erro, tamErro, "a chave publica do MySQL tem um formato DER que eu nao conheco.");
        return false;
    }

    Der dk(pkcs1, pkcs1N);
    uint8_t tm = 0; const uint8_t* cm = nullptr; size_t nm = 0;
    uint8_t te = 0; const uint8_t* ce = nullptr; size_t ne = 0;
    if (!dk.Ler(tm, cm, nm) || tm != 0x02 || !dk.Ler(te, ce, ne) || te != 0x02)
    {
        Diga(erro, tamErro, "nao achei o modulo e o expoente dentro da chave publica do MySQL.");
        return false;
    }
    // INTEGER DER pode vir com um 0x00 na frente para não parecer negativo.
    while (nm > 1 && cm[0] == 0) { ++cm; --nm; }
    while (ne > 1 && ce[0] == 0) { ++ce; --ne; }

    mod = NumDeBytes(cm, nm);
    exp = NumDeBytes(ce, ne);
    kBytes = nm;

    // Teto e piso. Chave absurdamente grande vira exponenciação de minutos
    // (negação de serviço vinda do servidor); chave pequena demais não cifra
    // senha nenhuma com segurança.
    if (kBytes < 128 || kBytes > 1024)
    {
        Diga(erro, tamErro, "o MySQL ofereceu uma chave RSA de %d bits, fora da faixa que eu "
                            "aceito (1024 a 8192). Recusei em vez de usar.", (int)(kBytes * 8));
        return false;
    }
    return true;
}

// MGF1 com SHA-1, como o RSA_PKCS1_OAEP_PADDING do OpenSSL usa por padrão —
// que é o que o servidor MySQL espera do outro lado.
void Mgf1(const uint8_t* semente, size_t nSemente, uint8_t* saida, size_t nSaida)
{
    uint32_t contador = 0;
    size_t feito = 0;
    while (feito < nSaida)
    {
        uint8_t c[4] = {
            (uint8_t)((contador >> 24) & 0xFF), (uint8_t)((contador >> 16) & 0xFF),
            (uint8_t)((contador >> 8) & 0xFF),  (uint8_t)(contador & 0xFF) };
        uint8_t h[20];
        Sha1 s; s.Iniciar(); s.Adicionar(semente, nSemente); s.Adicionar(c, 4); s.Fechar(h);
        size_t leva = (nSaida - feito) < 20 ? (nSaida - feito) : 20;
        memcpy(saida + feito, h, leva);
        feito += leva;
        ++contador;
    }
}

// EME-OAEP com SHA-1 (rótulo vazio), RFC 8017 §7.1.1.
bool OaepCodificar(const uint8_t* msg, size_t mLen, size_t k, uint8_t* em,
                   char* erro, int tamErro, const char** fonte)
{
    const size_t hLen = 20;
    if (mLen + 2 * hLen + 2 > k)
    {
        Diga(erro, tamErro, "a senha e longa demais para caber na cifragem RSA desta chave "
                            "(cabem %d bytes, a senha tem %d).",
                            (int)(k - 2 * hLen - 2), (int)mLen);
        return false;
    }
    // DB = lHash || PS(zeros) || 0x01 || M
    const size_t dbLen = k - hLen - 1;
    std::vector<uint8_t> db(dbLen, 0);
    Sha1De((const uint8_t*)"", 0, db.data());          // lHash = SHA1("")
    db[dbLen - mLen - 1] = 0x01;
    if (mLen) memcpy(db.data() + dbLen - mLen, msg, mLen);

    uint8_t semente[20];
    if (!AleatorioDoSistema(semente, hLen, fonte))
    {
        Diga(erro, tamErro, "nao consegui obter numeros aleatorios do sistema operacional para "
                            "cifrar a senha com seguranca, e me recusei a usar uma alternativa "
                            "fraca. Sem isso nao da para autenticar neste MySQL.");
        return false;
    }

    std::vector<uint8_t> mascara(dbLen);
    Mgf1(semente, hLen, mascara.data(), dbLen);
    for (size_t i = 0; i < dbLen; ++i) db[i] ^= mascara[i];

    uint8_t mascaraSemente[20];
    Mgf1(db.data(), dbLen, mascaraSemente, hLen);
    for (size_t i = 0; i < hLen; ++i) semente[i] ^= mascaraSemente[i];

    em[0] = 0x00;
    memcpy(em + 1, semente, hLen);
    memcpy(em + 1 + hLen, db.data(), dbLen);

    Limpar(semente, sizeof(semente));
    Limpar(mascaraSemente, sizeof(mascaraSemente));
    Limpar(db.data(), db.size());
    return true;
}

bool RsaCifrarOaep(const Num& mod, const Num& exp, size_t k,
                   const uint8_t* msg, size_t mLen,
                   std::vector<uint8_t>& saida, char* erro, int tamErro,
                   const char** fonte = nullptr)
{
    std::vector<uint8_t> em(k, 0);
    if (!OaepCodificar(msg, mLen, k, em.data(), erro, tamErro, fonte)) return false;
    Num m = NumDeBytes(em.data(), k);
    Limpar(em.data(), em.size());
    Num c = NumModExp(m, exp, mod);
    saida.assign(k, 0);
    if (!NumParaBytes(c, saida.data(), k))
    {
        Diga(erro, tamErro, "erro interno ao cifrar a senha (resultado RSA maior que a chave).");
        return false;
    }
    return true;
}

// ============================================================================
//  §4 · SOCKET COM PRAZO
// ============================================================================

#ifdef _WIN32
// WSAStartup uma vez por processo, e NUNCA WSACleanup.
//
// Isto é decisão consciente. O Winsock conta referências; chamar WSACleanup
// quando este plugin é descarregado derrubaria o Winsock para quem chamou
// WSAStartup depois de nós e ainda está usando — e o processo aqui é um
// servidor de jogo, cheio de gente usando rede. Deixar uma referência presa
// mantém o Winsock carregado até o processo morrer, que é exatamente o que se
// quer num servidor. O "vazamento" é de uma referência, não de memória.
void GarantirWinsock()
{
    static std::atomic<int> estado{0};       // 0 = não tentou, 1 = ok, 2 = falhou
    int e = estado.load(std::memory_order_acquire);
    if (e != 0) return;
    WSADATA d;
    int r = WSAStartup(MAKEWORD(2, 2), &d);
    estado.store(r == 0 ? 1 : 2, std::memory_order_release);
}
#endif

int64_t AgoraMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// ============================================================================
//  §5..§7 · O ESTADO DA CONEXÃO
// ============================================================================

// Capacidades do cliente. O que NÃO está aqui é tão importante quanto o que está:
//
//   CLIENT_LOCAL_FILES (0x80) — DESLIGADO DE PROPÓSITO.
//     Com ele, o servidor pode responder a QUALQUER consulta com um pedido
//     "me mande o arquivo X do seu disco", e um cliente obediente manda. É o
//     ataque conhecido do "rogue MySQL server": basta o dono do servidor de
//     jogo apontar o config.json para um endereço hostil (ou alguém no meio
//     do caminho responder por ele) e o processo entrega arquivos da máquina.
//     Desligado aqui, e ainda assim tratado como erro fatal no §7 se o
//     servidor tentar mesmo assim.
//
//   CLIENT_MULTI_STATEMENTS (0x10000) — DESLIGADO DE PROPÓSITO.
//     Com ele, um `'; DROP TABLE jogador; --` que escapasse do Escapar()
//     viraria um segundo comando. Sem ele, o servidor recusa a consulta
//     inteira com erro de sintaxe. É a segunda camada da defesa contra
//     injeção, e é a que não depende de eu ter escrito o escape sem bug.
enum : uint32_t {
    CAP_LONG_PASSWORD     = 0x00000001u,
    CAP_LONG_FLAG         = 0x00000004u,
    CAP_CONNECT_WITH_DB   = 0x00000008u,
    CAP_LOCAL_FILES       = 0x00000080u,
    CAP_PROTOCOL_41       = 0x00000200u,
    CAP_TRANSACTIONS      = 0x00002000u,
    CAP_SECURE_CONNECTION = 0x00008000u,
    CAP_MULTI_STATEMENTS  = 0x00010000u,
    CAP_PLUGIN_AUTH       = 0x00080000u,
    CAP_DEPRECATE_EOF     = 0x01000000u
};

// utf8mb4_general_ci. Existe no MySQL 5.5.3+, no 8.x e no MariaDB — o 255
// (utf8mb4_0900_ai_ci) só existe do MySQL 8 em diante e seria recusado por um
// 5.7. Ver INV-MYSQL-004 para por que o conjunto de caracteres importa tanto.
const uint8_t CHARSET_UTF8MB4 = 45;

enum class Metodo { NENHUM, NATIVO, CACHING_SHA2, SHA256 };

const char* NomeMetodo(Metodo m)
{
    switch (m)
    {
        case Metodo::NATIVO:       return "mysql_native_password";
        case Metodo::CACHING_SHA2: return "caching_sha2_password";
        case Metodo::SHA256:       return "sha256_password";
        default:                   return "(nenhum)";
    }
}

} // namespace anônimo

// ── o estado interno, escondido do header ────────────────────────────────
struct MySqlInterno
{
    TSoq     soq       = SOQ_NULO;
    uint8_t  seq       = 0;
    bool     quebrado  = false;     // fluxo dessincronizado: não reusar
    int      msConectar = 5000;
    int      msOperar   = 10000;
    size_t   tetoResposta = 64u * 1024u * 1024u;

    void  (*log)(void*, const char*) = nullptr;
    void*   logCtx = nullptr;

    std::string versaoServidor;
    std::string hostAtual;
    uint16_t    portaAtual = 0;
    uint32_t    capServidor = 0;
    uint32_t    capUsadas   = 0;

    uint64_t linhasAfetadas = 0;
    uint64_t ultimoId       = 0;
    unsigned codigoMysql    = 0;

    std::vector<std::string> colunas;
    std::vector<size_t>      comprimentos;
    std::vector<std::string> celulas;
    std::vector<const char*> ponteiros;

    std::vector<uint8_t> pacote;    // reaproveitado entre leituras

    // Guarda contra uso concorrente. Ver INV-MYSQL-003: duas threads na mesma
    // conexão intercalam pacotes e o número de sequência passa a bater por
    // acaso — a consulta de uma thread recebe a resposta da outra, com cara de
    // resposta certa. Isso NÃO é um erro que apareça em teste; aparece em
    // produção, raro, e some quando se procura. Melhor recusar.
    std::atomic<bool> emUso{false};

    // ── interrupção pedida por OUTRA thread (só no desligamento) ─────────────
    //
    // `interrompido` é definitivo: uma vez pedido, esta conexão não volta a
    // servir para nada — nem reconectando. Isso é de propósito. O único
    // chamador é Armazem::Fechar(), e reconectar durante o desligamento seria
    // abrir socket para um processo que está morrendo.
    std::atomic<bool> interrompido{false};

    // Protege APENAS a troca do número do descritor (fechar/anular) e o
    // shutdown() vindo de fora. Nunca é segurado durante I/O — se fosse,
    // Interromper() ficaria esperando justamente a operação que ele existe
    // para desbloquear. Ver o comentário de Interromper() em MySqlCliente.h
    // para o porquê de shutdown() e não close().
    std::mutex mtxSoq;

    void Registrar(const char* fmt, ...)
    {
        if (!log) return;
        char linha[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(linha, sizeof(linha), fmt, ap);
        va_end(ap);
        linha[sizeof(linha) - 1] = '\0';
        log(logCtx, linha);
    }

    void Derrubar()
    {
        // O fechamento entra na trava porque é ele que libera o número do
        // descritor para o SO reaproveitar. Interromper() segura a mesma trava
        // para fazer shutdown(); assim as duas nunca acontecem ao mesmo tempo,
        // e não existe a janela em que o shutdown cairia num descritor que já
        // virou outro socket deste processo.
        std::lock_guard<std::mutex> g(mtxSoq);
        if (soq != SOQ_NULO) { FECHAR_SOQ(soq); soq = SOQ_NULO; }
        quebrado = false;
        seq = 0;
    }
};

namespace
{

// Trava de reentrância com RAII.
class Reserva
{
public:
    explicit Reserva(MySqlInterno* i) : m_i(i), m_ok(false)
    {
        bool esperado = false;
        m_ok = m_i->emUso.compare_exchange_strong(esperado, true,
                                                  std::memory_order_acq_rel);
    }
    ~Reserva() { if (m_ok) m_i->emUso.store(false, std::memory_order_release); }
    bool Ok() const { return m_ok; }
private:
    MySqlInterno* m_i;
    bool m_ok;
};

// ── leitura/escrita com prazo absoluto ───────────────────────────────────
//
// POR QUE UM PRAZO ALÉM DO SO_RCVTIMEO
// O tempo-limite do socket vale por chamada de recv. Um servidor que mande
// UM byte a cada 9 segundos nunca dispara o SO_RCVTIMEO de 10 s, e mantém a
// thread presa para sempre entregando dado de conta-gotas. O prazo absoluto
// abaixo é o que fecha essa porta: ele mede a operação inteira, não a
// chamada. (INV-MYSQL-002)
bool LerBytes(MySqlInterno& I, uint8_t* p, size_t n, int64_t prazo,
              char* erro, int tamErro)
{
    while (n > 0)
    {
        if (AgoraMs() > prazo)
        {
            I.quebrado = true;
            Diga(erro, tamErro,
                 "o MySQL em '%s:%u' aceitou a conexao mas parou de responder no meio "
                 "(esperei %d ms). Pode ser rede instavel, o banco travado, ou um "
                 "firewall cortando a conexao pela metade.",
                 I.hostAtual.c_str(), (unsigned)I.portaAtual, I.msOperar);
            return false;
        }
#ifdef _WIN32
        int r = recv(I.soq, (char*)p, (int)(n > 0x40000000u ? 0x40000000u : n), 0);
#else
        ssize_t r = recv(I.soq, p, n, 0);
#endif
        if (r > 0) { p += r; n -= (size_t)r; continue; }
        if (r == 0)
        {
            I.quebrado = true;
            Diga(erro, tamErro,
                 "o MySQL em '%s:%u' fechou a conexao no meio da resposta. Se isso acontece "
                 "sempre no mesmo ponto, veja o log de erro do MySQL: costuma ser o servidor "
                 "matando a conexao (wait_timeout, max_allowed_packet) e nao a rede.",
                 I.hostAtual.c_str(), (unsigned)I.portaAtual);
            return false;
        }
        int e = ERRO_SOQ();
        if (e == SOQ_INTERROMP) continue;
#ifndef _WIN32
        if (e == EWOULDBLOCK) continue;      // SO_RCVTIMEO estourou: o prazo decide
#endif
        if (e == SOQ_BLOQUEIA) continue;     // idem, e no Windows é WSAEWOULDBLOCK
#ifdef _WIN32
        if (e == WSAETIMEDOUT) continue;
#endif
        I.quebrado = true;
        Diga(erro, tamErro,
             "erro de rede lendo do MySQL em '%s:%u' (codigo %d do sistema). A conexao "
             "foi perdida.", I.hostAtual.c_str(), (unsigned)I.portaAtual, e);
        return false;
    }
    return true;
}

bool EscreverBytes(MySqlInterno& I, const uint8_t* p, size_t n, int64_t prazo,
                   char* erro, int tamErro)
{
    while (n > 0)
    {
        if (AgoraMs() > prazo)
        {
            I.quebrado = true;
            Diga(erro, tamErro,
                 "o MySQL em '%s:%u' parou de aceitar dados (esperei %d ms enviando). "
                 "Normalmente e o banco travado ou a rede caindo.",
                 I.hostAtual.c_str(), (unsigned)I.portaAtual, I.msOperar);
            return false;
        }
#ifdef _WIN32
        int r = send(I.soq, (const char*)p, (int)(n > 0x40000000u ? 0x40000000u : n), 0);
#else
        ssize_t r = send(I.soq, p, n, MSG_NOSIGNAL);   // sem SIGPIPE: um pipe
                                                       // quebrado NÃO pode matar
                                                       // o processo do jogo
#endif
        if (r > 0) { p += r; n -= (size_t)r; continue; }
        int e = ERRO_SOQ();
        if (e == SOQ_INTERROMP) continue;
        if (e == SOQ_BLOQUEIA) continue;
#ifdef _WIN32
        if (e == WSAETIMEDOUT) continue;
#endif
        I.quebrado = true;
        Diga(erro, tamErro,
             "erro de rede enviando para o MySQL em '%s:%u' (codigo %d do sistema).",
             I.hostAtual.c_str(), (unsigned)I.portaAtual, e);
        return false;
    }
    return true;
}

// ── camada de pacote ─────────────────────────────────────────────────────
//
// Cabeçalho de 4 bytes: 3 de comprimento (little-endian) + 1 de sequência.
// Payload de exatamente 0xFFFFFF significa "continua no próximo pacote", e o
// último da corrente tem menos que isso — inclusive zero. Um payload de
// 32 MiB chega, então, como 3 pacotes: 16 MiB, 16 MiB e 0.
bool LerPacote(MySqlInterno& I, std::vector<uint8_t>& payload,
               int64_t prazo, char* erro, int tamErro)
{
    payload.clear();
    for (;;)
    {
        uint8_t cab[4];
        if (!LerBytes(I, cab, 4, prazo, erro, tamErro)) return false;
        uint32_t len = Le24(cab);
        uint8_t  s   = cab[3];

        if (s != I.seq)
        {
            // INV-MYSQL-003. Não tem conserto: o que vem depois no fio não é
            // de quem eu acho que é.
            I.quebrado = true;

            // DEFEITO REAL, achado rodando o teste em 18/08/2026: um servidor
            // HTTP na porta configurada caía aqui, e a mensagem falava em
            // "duas threads na mesma conexao" — mandando o dono do servidor
            // procurar exatamente onde não está o problema. Os bytes de
            // "HTTP/1.1 400" viram um cabeçalho com sequência 0x50 (o 'P').
            //
            // A distinção é objetiva: só a saudação inicial é esperada com
            // sequência 0. Se a divergência acontece aí, ninguém
            // dessincronizou coisa nenhuma — o que respondeu não é um MySQL.
            if (I.seq == 0)
                Diga(erro, tamErro,
                     "o que atende em '%s:%u' nao fala o protocolo do MySQL (a primeira "
                     "resposta veio com numero de sequencia %u, e o MySQL sempre comeca em 0). "
                     "Quase sempre e' porta errada: algum outro programa atende nessa porta. "
                     "Confira MysqlPort no config.json — o padrao do MySQL e 3306.",
                     I.hostAtual.c_str(), (unsigned)I.portaAtual, (unsigned)s);
            else
                // SEGUNDO DEFEITO DE MENSAGEM, achado em 18/08/2026 derrubando
                // um proxy no meio de uma operação (testes/teste_banco.cpp,
                // cenário 6): esta linha abria com "duas threads usam a MESMA
                // conexao" — e isso é IMPOSSÍVEL aqui. Uso concorrente de
                // verdade é barrado pela reserva `emUso`, que tem mensagem
                // própria e nem chega neste ponto. Ou seja: a primeira causa
                // oferecida ao dono do servidor era a única que ele podia
                // descartar de antemão, e para descobrir isso ele teria de ler
                // este arquivo.
                //
                // O que realmente produz isto na máquina de um dono: a conexão
                // foi cortada no meio de uma resposta (banco reiniciado, KILL,
                // proxy/túnel caindo, NAT expirando a sessão) ou algo entre as
                // duas pontas mexe no tráfego. Estas vêm primeiro agora.
                Diga(erro, tamErro,
                     "o fluxo com o MySQL em '%s:%u' saiu de ordem (esperava o pacote %u e veio "
                     "o %u). Quase sempre e a conexao cortada no meio de uma resposta: o banco "
                     "reiniciou, alguem deu KILL na sessao, ou um proxy/tunel/NAT no meio do "
                     "caminho derrubou a ligacao. Tambem acontece com firewall que inspeciona e "
                     "altera o trafego. Derrubei a conexao em vez de ler dado trocado, e vou "
                     "reconectar sozinho.",
                     I.hostAtual.c_str(), (unsigned)I.portaAtual, (unsigned)I.seq, (unsigned)s);
            return false;
        }
        I.seq = (uint8_t)(s + 1);

        if (payload.size() + len > I.tetoResposta)
        {
            I.quebrado = true;
            Diga(erro, tamErro,
                 "o MySQL em '%s:%u' esta mandando uma resposta maior que o teto de %s bytes. "
                 "Parei de ler: continuar significaria alocar memoria sem limite e derrubar o "
                 "servidor de jogo por falta de memoria.",
                 I.hostAtual.c_str(), (unsigned)I.portaAtual,
                 Dec((uint64_t)I.tetoResposta).c_str());
            return false;
        }

        size_t antes = payload.size();
        payload.resize(antes + len);
        if (len && !LerBytes(I, payload.data() + antes, len, prazo, erro, tamErro))
            return false;

        if (len < 0xFFFFFFu) return true;
    }
}

bool EnviarPacote(MySqlInterno& I, const uint8_t* p, size_t n,
                  int64_t prazo, char* erro, int tamErro)
{
    size_t restante = n;
    for (;;)
    {
        size_t bloco = restante > 0xFFFFFFu ? 0xFFFFFFu : restante;
        uint8_t cab[4] = {
            (uint8_t)(bloco & 0xFF),
            (uint8_t)((bloco >> 8) & 0xFF),
            (uint8_t)((bloco >> 16) & 0xFF),
            I.seq };
        I.seq = (uint8_t)(I.seq + 1);
        if (!EscreverBytes(I, cab, 4, prazo, erro, tamErro)) return false;
        if (bloco && !EscreverBytes(I, p, bloco, prazo, erro, tamErro)) return false;
        p += bloco;
        restante -= bloco;
        // Se o bloco encheu, a corrente continua — mesmo que não sobre nada,
        // e aí o próximo pacote tem comprimento zero. Sem esse pacote vazio o
        // servidor fica esperando o resto para sempre.
        if (bloco < 0xFFFFFFu) return true;
    }
}

// ── pacotes de resposta ──────────────────────────────────────────────────

bool EhErr(const std::vector<uint8_t>& p) { return !p.empty() && p[0] == 0xFF; }

// EOF tem de ser distinguido de uma LINHA cujo primeiro valor começa com
// 0xFE. O critério é o tamanho: 0xFE como prefixo de comprimento variável
// arrasta 8 bytes atrás, então uma linha assim tem 9 bytes ou mais. Pacote
// com 0xFE e menos de 9 bytes só pode ser EOF. Confundir os dois faz o
// leitor parar no meio do resultado — e o fluxo fica desalinhado.
bool EhEof(const std::vector<uint8_t>& p) { return p.size() < 9 && !p.empty() && p[0] == 0xFE; }

// ERR: 0xFF + 2 bytes de código + (se protocolo 4.1) '#' + 5 de SQLSTATE + texto.
// Antes da autenticação o servidor ainda não sabe que falamos 4.1, então o
// '#' pode não vir. O teste do '#' resolve os dois casos.
void LerErr(const std::vector<uint8_t>& p, unsigned& codigo, std::string& estado,
            std::string& msg)
{
    codigo = 0; estado.clear(); msg.clear();
    Cursor c(p.data(), p.size());
    uint8_t b = 0;
    if (!c.U8(b)) return;
    uint16_t cod = 0;
    if (!c.U16(cod)) return;
    codigo = cod;
    if (c.Restam() >= 6)
    {
        const uint8_t* q = nullptr;
        // espia sem consumir: só o próximo byte
        Cursor espia = c;
        uint8_t marca = 0;
        if (espia.U8(marca) && marca == '#')
        {
            c.U8(marca);
            if (c.Bytes(5, &q)) estado.assign((const char*)q, 5);
        }
    }
    const uint8_t* q = nullptr; size_t n = 0;
    c.Resto(&q, n);
    if (n) msg.assign((const char*)q, n);
}

void LerOk(const std::vector<uint8_t>& p, uint64_t& afetadas, uint64_t& ultimoId)
{
    afetadas = 0; ultimoId = 0;
    Cursor c(p.data(), p.size());
    uint8_t b = 0;
    if (!c.U8(b)) return;
    bool nulo = false;
    if (!c.VarInt(afetadas, nulo)) { afetadas = 0; return; }
    if (!c.VarInt(ultimoId, nulo)) { ultimoId = 0; return; }
}

// Traduz o erro do servidor para uma frase que o dono do servidor entenda,
// mantendo SEMPRE o código e o texto originais no fim — o código é o que
// permite a ele pesquisar, e o texto é o que permite a mim não ter de prever
// todos os erros do MySQL.
void ExplicarErroDoServidor(unsigned codigo, const std::string& msg,
                            const char* usuario, const char* banco,
                            char* erro, int tamErro)
{
    switch (codigo)
    {
    case 1045:  // ER_ACCESS_DENIED_ERROR
        Diga(erro, tamErro,
             "o MySQL recusou o login do usuario '%s': usuario ou senha errados, ou esse "
             "usuario nao tem permissao de entrar a partir desta maquina. Confira MysqlUser "
             "e MysqlPass no config.json. (erro %u do MySQL: %s)",
             usuario ? usuario : "", codigo, msg.c_str());
        return;
    case 1044:  // ER_DBACCESS_DENIED_ERROR
        // O MySQL responde 1044 tanto para "o banco existe e voce nao pode"
        // quanto para "o banco nao existe e voce nao poderia nem saber" — ele
        // esconde a diferenca de proposito, para nao revelar quais bancos
        // existem a quem nao tem permissao. Medido no MySQL 8.4.11 em
        // 18/08/2026: root recebe 1049 (banco inexistente) e um usuario comum
        // recebe 1044 para o MESMO banco inexistente.
        //
        // Por isso a mensagem traz OS DOIS comandos. Dizer so "de GRANT" faria
        // o dono do servidor tentar dar permissao num banco que nao existe, o
        // GRANT ate' funcionaria (o MySQL cria a permissao mesmo assim) e a
        // conexao continuaria falhando — sem ele entender por que.
        Diga(erro, tamErro,
             "o usuario '%s' entrou no MySQL, mas nao conseguiu abrir o banco '%s'. Sao duas "
             "causas possiveis e o MySQL nao diz qual: ou o banco nao existe, ou o usuario nao "
             "tem permissao nele. Rode as duas coisas no MySQL:  "
             "CREATE DATABASE IF NOT EXISTS `%s` CHARACTER SET utf8mb4;  e  "
             "GRANT ALL ON `%s`.* TO '%s'@'%%'; FLUSH PRIVILEGES;  (erro %u do MySQL: %s)",
             usuario ? usuario : "", banco ? banco : "", banco ? banco : "",
             banco ? banco : "", usuario ? usuario : "", codigo, msg.c_str());
        return;
    case 1049:  // ER_BAD_DB_ERROR
        Diga(erro, tamErro,
             "o banco '%s' nao existe neste MySQL. Crie com:  "
             "CREATE DATABASE `%s` CHARACTER SET utf8mb4;  (erro %u do MySQL: %s)",
             banco ? banco : "", banco ? banco : "", codigo, msg.c_str());
        return;
    case 1130:  // ER_HOST_NOT_PRIVILEGED
        Diga(erro, tamErro,
             "o MySQL nao aceita conexao vinda desta maquina. O usuario provavelmente esta "
             "cadastrado so como '%s'@'localhost'. (erro %u do MySQL: %s)",
             usuario ? usuario : "", codigo, msg.c_str());
        return;
    case 1129:  // ER_HOST_IS_BLOCKED
        Diga(erro, tamErro,
             "o MySQL BLOQUEOU esta maquina por excesso de erros de conexao. Rode no MySQL:  "
             "FLUSH HOSTS;  e conserte a causa antes de tentar de novo. (erro %u do MySQL: %s)",
             codigo, msg.c_str());
        return;
    case 1040:  // ER_CON_COUNT_ERROR
        Diga(erro, tamErro,
             "o MySQL esta com todas as conexoes ocupadas (max_connections). (erro %u do "
             "MySQL: %s)", codigo, msg.c_str());
        return;
    case 1203:
        Diga(erro, tamErro,
             "o usuario '%s' ja atingiu o limite de conexoes simultaneas dele no MySQL. "
             "(erro %u do MySQL: %s)", usuario ? usuario : "", codigo, msg.c_str());
        return;
    default:
        Diga(erro, tamErro, "o MySQL respondeu com erro %u: %s", codigo, msg.c_str());
        return;
    }
}

} // namespace anônimo


// ============================================================================
//  §6 · HANDSHAKE E AUTENTICAÇÃO
// ============================================================================
namespace
{

struct Handshake
{
    std::string versao;
    uint32_t    capacidades = 0;
    uint8_t     sal[20];
    size_t      nSal = 0;
    std::string plugin;
};

bool LerHandshake(MySqlInterno& I, const std::vector<uint8_t>& p,
                  Handshake& h, char* erro, int tamErro)
{
    Cursor c(p.data(), p.size());
    uint8_t versaoProtocolo = 0;
    if (!c.U8(versaoProtocolo))
    {
        Diga(erro, tamErro, "o servidor em '%s:%u' respondeu um pacote vazio no lugar da "
                            "saudacao do MySQL.", I.hostAtual.c_str(), (unsigned)I.portaAtual);
        return false;
    }
    if (versaoProtocolo != 10)
    {
        Diga(erro, tamErro,
             "o que atende em '%s:%u' nao fala o protocolo do MySQL 4.1+ (mandou versao de "
             "protocolo %u, esperado 10). Confira se a porta esta certa: e comum apontar sem "
             "querer para outro servico.",
             I.hostAtual.c_str(), (unsigned)I.portaAtual, (unsigned)versaoProtocolo);
        return false;
    }
    if (!c.StrNul(h.versao))
    {
        Diga(erro, tamErro, "a saudacao do MySQL veio truncada (sem a versao do servidor).");
        return false;
    }
    // O MariaDB >= 10 anuncia "5.5.5-10.11.x-MariaDB" porque clientes antigos
    // não entendiam versão 10.x. O prefixo é enfeite de compatibilidade.
    if (h.versao.compare(0, 6, "5.5.5-") == 0) h.versao = h.versao.substr(6);

    uint32_t threadId = 0;
    if (!c.U32(threadId)) { Diga(erro, tamErro, "saudacao do MySQL truncada (id da conexao)."); return false; }

    const uint8_t* sal1 = nullptr;
    if (!c.Bytes(8, &sal1)) { Diga(erro, tamErro, "saudacao do MySQL truncada (sal parte 1)."); return false; }
    memcpy(h.sal, sal1, 8);
    h.nSal = 8;

    uint8_t filler = 0;
    c.U8(filler);                                   // sempre 0x00
    (void)filler;

    uint16_t capBaixo = 0;
    if (!c.U16(capBaixo)) { Diga(erro, tamErro, "saudacao do MySQL truncada (capacidades)."); return false; }
    h.capacidades = capBaixo;

    if (c.Restam() > 0)
    {
        uint8_t charset = 0; uint16_t status = 0, capAlto = 0; uint8_t tamAuth = 0;
        c.U8(charset);                              // conjunto padrão do servidor
        c.U16(status);                              // flags de status da conexão
        (void)charset; (void)status;                // lidos para andar o cursor
        c.U16(capAlto);
        h.capacidades |= ((uint32_t)capAlto << 16);
        c.U8(tamAuth);
        c.Pular(10);                                // reservado, tudo zero

        // A parte 2 do sal tem MAX(13, tamAuth-8) bytes; o último é um '\0'
        // de enfeite. O sal útil são 20 bytes: 8 + 12.
        size_t n2 = (tamAuth > 8) ? (size_t)(tamAuth - 8) : 0;
        if (n2 < 13) n2 = 13;
        const uint8_t* sal2 = nullptr;
        if (c.Bytes(n2, &sal2))
        {
            size_t leva = n2 > 12 ? 12 : n2;
            memcpy(h.sal + 8, sal2, leva);
            h.nSal = 8 + leva;
        }
        if (h.capacidades & CAP_PLUGIN_AUTH) c.StrNul(h.plugin);
    }
    if (h.plugin.empty()) h.plugin = "mysql_native_password";

    if (h.nSal != 20)
    {
        Diga(erro, tamErro,
             "o MySQL em '%s:%u' mandou um sal de autenticacao de %d bytes (o protocolo usa 20). "
             "Nao vou tentar autenticar com dado que nao entendi.",
             I.hostAtual.c_str(), (unsigned)I.portaAtual, (int)h.nSal);
        return false;
    }
    return true;
}

// mysql_native_password:  SHA1(senha) XOR SHA1( sal || SHA1(SHA1(senha)) )
void RespostaNativa(const char* senha, const uint8_t sal[20], std::vector<uint8_t>& saida)
{
    saida.clear();
    if (!senha || !*senha) return;                  // senha vazia = resposta vazia
    uint8_t e1[20], e2[20], e3[20];
    Sha1De((const uint8_t*)senha, strlen(senha), e1);
    Sha1De(e1, 20, e2);
    Sha1De2(sal, 20, e2, 20, e3);
    saida.resize(20);
    for (int i = 0; i < 20; ++i) saida[i] = (uint8_t)(e1[i] ^ e3[i]);
    Limpar(e1, sizeof(e1)); Limpar(e2, sizeof(e2)); Limpar(e3, sizeof(e3));
}

// caching_sha2_password, caminho rápido:
//   SHA256(senha) XOR SHA256( SHA256(SHA256(senha)) || sal )
void RespostaCachingSha2(const char* senha, const uint8_t sal[20], std::vector<uint8_t>& saida)
{
    saida.clear();
    if (!senha || !*senha) return;
    uint8_t m1[32], m1h[32], m2[32];
    Sha256De((const uint8_t*)senha, strlen(senha), m1);
    Sha256De(m1, 32, m1h);
    Sha256De2(m1h, 32, sal, 20, m2);
    saida.resize(32);
    for (int i = 0; i < 32; ++i) saida[i] = (uint8_t)(m1[i] ^ m2[i]);
    Limpar(m1, sizeof(m1)); Limpar(m1h, sizeof(m1h)); Limpar(m2, sizeof(m2));
}

Metodo MetodoDoNome(const std::string& n)
{
    if (n == "mysql_native_password")  return Metodo::NATIVO;
    if (n == "caching_sha2_password")  return Metodo::CACHING_SHA2;
    if (n == "sha256_password")        return Metodo::SHA256;
    return Metodo::NENHUM;
}

bool CalcularResposta(Metodo m, const char* senha, const uint8_t sal[20],
                      std::vector<uint8_t>& saida)
{
    switch (m)
    {
        case Metodo::NATIVO:       RespostaNativa(senha, sal, saida);       return true;
        case Metodo::CACHING_SHA2: RespostaCachingSha2(senha, sal, saida);  return true;
        case Metodo::SHA256:
            // sha256_password sem TLS não manda escrambleo nenhum na primeira
            // volta: ou a senha é vazia (manda 1 byte 0x00) ou pede a chave
            // pública (1 byte 0x01) e cifra depois.
            saida.clear();
            if (!senha || !*senha) saida.push_back(0x00);
            else                   saida.push_back(0x01);
            return true;
        default: return false;
    }
}

// A frase que a exigência 3 da tarefa pede — com uma correção que só apareceu
// ao rodar contra o servidor de teste:
//
//   O MySQL 8.4 REMOVEU o mysql_native_password. Mandar o dono rodar
//   `ALTER USER ... IDENTIFIED WITH mysql_native_password` num 8.4 faz o
//   MySQL responder "ERROR 1524 (HY000): Plugin 'mysql_native_password' is
//   not loaded" — ou seja, a mensagem de ajuda mandaria ele num beco. Uma
//   mensagem de erro que leva a uma acao que falha e' pior que nenhuma: ele
//   perde a tarde achando que fez errado.
//
// Por isso a dica é escolhida pela versão que o servidor anunciou no
// handshake, que já está na mão neste ponto.
void DicaDeAutenticacao(const std::string& versao, const char* usuario,
                        char* saida, int tam)
{
    bool mariadb = versao.find("MariaDB") != std::string::npos
                || versao.find("mariadb") != std::string::npos;
    int maior = 0, menor = 0;
    (void)sscanf(versao.c_str(), "%d.%d", &maior, &menor);

    if (!mariadb && (maior > 8 || (maior == 8 && menor >= 4)))
    {
        snprintf(saida, (size_t)tam,
            "Este MySQL e' %s, e a partir do 8.4 o metodo antigo "
            "(mysql_native_password) NAO EXISTE MAIS — nao adianta tentar trocar o usuario "
            "para ele. Caminhos que funcionam neste servidor: (a) deixar o MySQL na mesma "
            "maquina ou numa rede privada e usar o metodo padrao mesmo, que e' o que este "
            "plugin faz sozinho; (b) se voce precisa mesmo do metodo antigo, subir o MySQL "
            "com a opcao  --mysql-native-password=ON  e so entao rodar  "
            "ALTER USER '%s'@'%%' IDENTIFIED WITH mysql_native_password BY 'sua-senha';",
            versao.c_str(), usuario ? usuario : "seu_usuario");
    }
    else
    {
        snprintf(saida, (size_t)tam,
            "No MySQL, rode:  ALTER USER '%s'@'%%' IDENTIFIED WITH mysql_native_password BY "
            "'sua-senha';  seguido de  FLUSH PRIVILEGES;  (servidor: %s)",
            usuario ? usuario : "seu_usuario", versao.c_str());
    }
    saida[tam - 1] = '\0';
}

} // namespace anônimo


// ============================================================================
//  §8 · ESCAPE E ASPAS
// ============================================================================

std::string MySqlCliente::Escapar(const char* s)
{
    return s ? Escapar(s, strlen(s)) : std::string();
}

std::string MySqlCliente::Escapar(const char* s, size_t n)
{
    // A tabela é a mesma do mysql_real_escape_string quando o servidor NÃO
    // está em NO_BACKSLASH_ESCAPES — e Conectar() garante que não está
    // (INV-MYSQL-004). Cada caso abaixo existe por um motivo concreto:
    //
    //   0x00  '\0'  termina string em C dentro do servidor; e um NUL no meio
    //               de um literal quebra o parser do MySQL
    //   '\n'        quebra de linha em log e em comando de linha de comando
    //   '\r'        idem
    //   '\\'        TEM de vir escapada, senão ela escapa a aspa seguinte e o
    //               atacante fecha o literal:  \' vira barra + fim de string
    //   '\''        fecha o literal — o vetor de injeção clássico
    //   '"'         inofensivo dentro de aspas simples, escapado por simetria
    //               com o cliente oficial (e porque ANSI_QUOTES existe)
    //   0x1A        Ctrl+Z: no Windows, fim de arquivo em modo texto; corta
    //               dump e script pela metade
    //
    // Por que byte a byte é seguro aqui: em UTF-8 todo byte de continuação é
    // >= 0x80, então nenhum caractere multibyte pode terminar em 0x5C ('\')
    // nem em 0x27 ('\''). Em GBK, SJIS e BIG5 PODE — e é a injeção clássica
    // do "\xbf\x27". Conectar() força utf8mb4 e confere. Sem essa garantia
    // esta função seria insegura, e por isso ela mora ao lado dela.
    std::string r;
    r.reserve(n + n / 8 + 8);
    for (size_t i = 0; i < n; ++i)
    {
        unsigned char c = (unsigned char)s[i];
        switch (c)
        {
            case 0x00: r += "\\0";  break;
            case '\n': r += "\\n";  break;
            case '\r': r += "\\r";  break;
            case '\\': r += "\\\\"; break;
            case '\'': r += "\\'";  break;
            case '"':  r += "\\\""; break;
            case 0x1A: r += "\\Z";  break;
            default:   r += (char)c; break;
        }
    }
    return r;
}

std::string MySqlCliente::Citar(const char* s)
{
    return s ? Citar(s, strlen(s)) : std::string("NULL");
}

std::string MySqlCliente::Citar(const char* s, size_t n)
{
    // Ponteiro nulo vira NULL (sem aspas) porque é isso que o chamador quer
    // dizer. `Citar(nullptr)` devolvendo `''` transformaria "não tem valor"
    // em "tem valor vazio" — coisas diferentes no banco, e a diferença só
    // aparece meses depois num relatório errado.
    if (!s) return std::string("NULL");
    std::string r;
    r.reserve(n + n / 8 + 8);
    r += '\'';
    r += Escapar(s, n);
    r += '\'';
    return r;
}


// ============================================================================
//  §9 · A CLASSE
// ============================================================================

MySqlCliente::MySqlCliente() : m_i(new MySqlInterno()) {}

MySqlCliente::~MySqlCliente()
{
    if (m_i) { m_i->Derrubar(); delete m_i; m_i = nullptr; }
}

void MySqlCliente::DefinirTempos(int msConectar, int msOperar)
{
    if (msConectar > 0) m_i->msConectar = msConectar;
    if (msOperar   > 0) m_i->msOperar   = msOperar;
}

void MySqlCliente::DefinirTetoResposta(size_t bytes)
{
    // Piso de 64 KiB: teto menor que uma resposta legítima transformaria o
    // ajuste numa guarda que reprova o certo.
    if (bytes >= 64u * 1024u) m_i->tetoResposta = bytes;
}

void MySqlCliente::DefinirLog(void (*log)(void*, const char*), void* ctx)
{
    m_i->log = log;
    m_i->logCtx = ctx;
}

bool MySqlCliente::Conectado() const
{
    return m_i->soq != SOQ_NULO && !m_i->quebrado;
}

void MySqlCliente::Desconectar()
{
    if (m_i->soq == SOQ_NULO) return;
    // Vale a mesma regra do resto da classe: uma conexão, uma thread. Se
    // outra thread estiver no meio de uma consulta, o COM_QUIT abaixo
    // atropelaria o fluxo dela — então nesse caso não se manda nada e vai
    // direto para o fechamento do socket, que é o pedido do chamador.
    Reserva r(m_i);
    if (!r.Ok()) { m_i->Derrubar(); return; }
    // COM_QUIT (0x01) por educação: sem ele o MySQL registra "Aborted
    // connection" no log de erro dele, e o dono do servidor vai achar que tem
    // problema de rede quando não tem. Se falhar, não importa — vamos fechar
    // o socket de qualquer jeito.
    m_i->seq = 0;
    const uint8_t quit[1] = { 0x01 };
    int64_t prazo = AgoraMs() + 500;
    EnviarPacote(*m_i, quit, 1, prazo, nullptr, 0);
    m_i->Derrubar();
}

void MySqlCliente::Interromper()
{
    // A ordem importa: marca ANTES de mexer no socket. Quem estiver no meio de
    // uma operação vai receber o erro do shutdown e, ao tentar o passo
    // seguinte, encontrar a marca já posta — em vez de decidir com um estado
    // que ainda diz "tudo bem".
    m_i->interrompido.store(true, std::memory_order_release);

    std::lock_guard<std::mutex> g(m_i->mtxSoq);
    m_i->quebrado = true;
    if (m_i->soq == SOQ_NULO) return;
#ifdef _WIN32
    ::shutdown(m_i->soq, SD_BOTH);
#else
    ::shutdown(m_i->soq, SHUT_RDWR);
#endif
}

namespace
{

// Abre o socket TCP com prazo de conexão. Devolve false com texto pronto.
bool AbrirSocket(MySqlInterno& I, const char* host, uint16_t porta,
                 char* erro, int tamErro)
{
#ifdef _WIN32
    GarantirWinsock();
#endif
    char servico[16];
    snprintf(servico, sizeof(servico), "%u", (unsigned)porta);

    struct addrinfo dica;
    memset(&dica, 0, sizeof(dica));
    dica.ai_family   = AF_UNSPEC;      // IPv4 ou IPv6, o que existir
    dica.ai_socktype = SOCK_STREAM;
    dica.ai_protocol = IPPROTO_TCP;

    // PRIMEIRA TENTATIVA: tratar o host como IP literal (AI_NUMERICHOST).
    //
    // POR QUE ISSO IMPORTA: getaddrinfo NÃO tem tempo-limite portátil. Um DNS
    // que engole o pacote prende esta thread pelo tempo do resolvedor do
    // sistema (30 s ou mais no Windows), e nenhum ajuste desta classe alcança
    // isso. A maioria esmagadora dos config.json reais tem um IP ou
    // "localhost" — e com AI_NUMERICHOST o IP resolve na hora, sem tocar em
    // DNS nenhum. Sobra o caso do nome de verdade, onde a espera existe e
    // está declarada como risco residual no topo deste arquivo.
    struct addrinfo* lista = nullptr;
    struct addrinfo dicaNum = dica;
    dicaNum.ai_flags = AI_NUMERICHOST;
    int r = getaddrinfo(host, servico, &dicaNum, &lista);
    if (r != 0)
    {
        lista = nullptr;
        r = getaddrinfo(host, servico, &dica, &lista);
    }
    if (r != 0 || !lista)
    {
        Diga(erro, tamErro,
             "nao consegui resolver o endereco '%s'. Confira se o nome esta escrito certo em "
             "MysqlHost no config.json; se for um nome (e nao um IP), confira tambem se esta "
             "maquina tem DNS funcionando. Se o MySQL esta na mesma maquina, use 127.0.0.1.",
             host);
        return false;
    }

    int64_t prazo = AgoraMs() + I.msConectar;
    int ultimoErro = 0;
    for (struct addrinfo* a = lista; a; a = a->ai_next)
    {
        TSoq s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == SOQ_NULO) { ultimoErro = ERRO_SOQ(); continue; }

        // não-bloqueante só para o connect: é a única forma portátil de dar
        // prazo ao connect (o SO tem prazo próprio, de dezenas de segundos)
#ifdef _WIN32
        u_long naoBloqueia = 1;
        ioctlsocket(s, FIONBIO, &naoBloqueia);
#else
        int fl = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif
        int rc = connect(s, a->ai_addr, (int)a->ai_addrlen);
        bool ok = (rc == 0);
        if (!ok)
        {
            int e = ERRO_SOQ();
#ifdef _WIN32
            bool emAndamento = (e == WSAEWOULDBLOCK || e == WSAEINPROGRESS);
#else
            bool emAndamento = (e == EINPROGRESS || e == EWOULDBLOCK);
#endif
            if (emAndamento)
            {
                int64_t falta = prazo - AgoraMs();
                if (falta < 0) falta = 0;
                // ESPERA COM PRAZO PELO FIM DO connect().
                //
                // No Linux e' poll(), NAO select(): o select() do POSIX guarda
                // os descritores num mapa de bits de FD_SETSIZE (1024) posicoes
                // indexado pelo VALOR do descritor, e um processo que ja tenha
                // 1024 arquivos abertos recebe um descritor >= 1024 — aí o
                // FD_SET escreve fora do fd_set. E comportamento indefinido,
                // silencioso, e so acontece em servidor carregado: exatamente o
                // defeito que nao aparece em teste. O poll() nao tem esse teto.
                //
                // No Windows o fd_set e' um VETOR de SOCKETs, sem indexacao
                // pelo valor, e o WSAPoll so existe do Vista em diante — la o
                // select() e' a escolha certa.
                int sel;
                bool pronto = false;
#ifdef _WIN32
                fd_set escrita, excecao;
                FD_ZERO(&escrita); FD_SET(s, &escrita);
                FD_ZERO(&excecao); FD_SET(s, &excecao);
                struct timeval tv;
                tv.tv_sec  = (long)(falta / 1000);
                tv.tv_usec = (long)((falta % 1000) * 1000);
                sel = select((int)s + 1, nullptr, &escrita, &excecao, &tv);
                pronto = (sel > 0);
#else
                struct pollfd pf;
                pf.fd = s;
                pf.events = POLLOUT;
                pf.revents = 0;
                sel = poll(&pf, 1, (int)falta);
                pronto = (sel > 0);
#endif
                if (pronto)
                {
                    int soErro = 0;
                    socklen_t tamSo = sizeof(soErro);
                    if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&soErro, &tamSo) == 0
                        && soErro == 0)
                        ok = true;
                    else
                        ultimoErro = soErro ? soErro : ERRO_SOQ();
                }
                else if (sel == 0)
                {
                    ultimoErro = -1;                 // -1 = estourou o prazo
                }
                else
                {
                    ultimoErro = ERRO_SOQ();
                }
            }
            else ultimoErro = e;
        }

        if (!ok) { FECHAR_SOQ(s); continue; }

        // volta ao modo bloqueante: daqui em diante quem manda no tempo é o
        // SO_RCVTIMEO/SO_SNDTIMEO mais o prazo absoluto de cada operação
#ifdef _WIN32
        u_long bloqueia = 0;
        ioctlsocket(s, FIONBIO, &bloqueia);
        DWORD ms = (DWORD)I.msOperar;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof(ms));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&ms, sizeof(ms));
#else
        int fl2 = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, fl2 & ~O_NONBLOCK);
        struct timeval tvo;
        tvo.tv_sec  = I.msOperar / 1000;
        tvo.tv_usec = (I.msOperar % 1000) * 1000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tvo, sizeof(tvo));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tvo, sizeof(tvo));
#endif
        // O protocolo é pergunta-resposta curta. Sem TCP_NODELAY, o Nagle
        // segura o pacote esperando companhia e cada consulta ganha até
        // 40 ms de atraso à toa.
        int um = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&um, sizeof(um));
        setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (const char*)&um, sizeof(um));

        freeaddrinfo(lista);
        I.soq = s;
        I.seq = 0;
        I.quebrado = false;
        return true;
    }

    freeaddrinfo(lista);
    if (ultimoErro == -1)
    {
        Diga(erro, tamErro,
             "nao consegui conectar em '%s:%u' em %d ms. O MySQL pode estar desligado, a "
             "porta pode estar bloqueada por firewall, ou o endereco pode estar errado. "
             "Teste da mesma maquina com:  telnet %s %u",
             host, (unsigned)porta, I.msConectar, host, (unsigned)porta);
    }
    else
    {
#ifdef _WIN32
        bool recusou = (ultimoErro == WSAECONNREFUSED);
#else
        bool recusou = (ultimoErro == ECONNREFUSED);
#endif
        if (recusou)
            Diga(erro, tamErro,
                 "a maquina '%s' respondeu, mas nao ha nada escutando na porta %u. O MySQL "
                 "esta parado, ou esta escutando em outra porta (confira MysqlPort no "
                 "config.json — o padrao e 3306).",
                 host, (unsigned)porta);
        else
            Diga(erro, tamErro,
                 "nao consegui conectar em '%s:%u' (codigo %d do sistema).",
                 host, (unsigned)porta, ultimoErro);
    }
    return false;
}

// Executa um comando simples esperando OK. Usado pelo endurecimento pós-login.
bool ComandoSimples(MySqlInterno& I, const char* sql, char* erro, int tamErro);
bool ConsultaUmValor(MySqlInterno& I, const char* sql, std::string& valor,
                     bool& achou, char* erro, int tamErro);

} // namespace anônimo


bool MySqlCliente::Conectar(const char* host, uint16_t porta, const char* usuario,
                            const char* senha, const char* banco, char* erro, int tamErro)
{
    if (erro && tamErro > 0) erro[0] = '\0';
    Reserva r(m_i);
    if (!r.Ok())
    {
        Diga(erro, tamErro,
             "esta conexao com o MySQL ja esta sendo usada por outra thread. Cada thread "
             "precisa da sua propria conexao — compartilhar uma so embaralha as respostas.");
        return false;
    }
    // Interromper() é definitivo (ver MySqlCliente.h): quem pediu foi o
    // desligamento do servidor. Abrir socket novo aqui seria conectar a partir
    // de um processo que está morrendo, e ainda faria o join do Armazem esperar
    // por uma conexão inteira.
    if (m_i->interrompido.load(std::memory_order_acquire))
    {
        Diga(erro, tamErro, "o servidor esta desligando; nao abro conexao nova com o MySQL.");
        return false;
    }

    try
    {
        if (!host || !*host)
        {
            Diga(erro, tamErro, "MysqlHost esta vazio no config.json. Preencha com o endereco "
                                "do MySQL (use 127.0.0.1 se ele estiver nesta mesma maquina).");
            return false;
        }
        if (!usuario || !*usuario)
        {
            Diga(erro, tamErro, "MysqlUser esta vazio no config.json.");
            return false;
        }
        if (porta == 0)
        {
            Diga(erro, tamErro, "MysqlPort esta zerado no config.json. A porta padrao do MySQL "
                                "e 3306.");
            return false;
        }

        if (m_i->soq != SOQ_NULO) m_i->Derrubar();
        m_i->codigoMysql = 0;
        m_i->hostAtual = host;
        m_i->portaAtual = porta;

        if (!AbrirSocket(*m_i, host, porta, erro, tamErro)) return false;

        int64_t prazo = AgoraMs() + m_i->msOperar;

        // ── 1. saudação do servidor ─────────────────────────────────────
        std::vector<uint8_t>& p = m_i->pacote;
        if (!LerPacote(*m_i, p, prazo, erro, tamErro)) { m_i->Derrubar(); return false; }

        if (EhErr(p))
        {
            // Acontece de verdade: "Host is blocked", "Too many connections".
            unsigned cod = 0; std::string est, msg;
            LerErr(p, cod, est, msg);
            m_i->codigoMysql = cod;
            ExplicarErroDoServidor(cod, msg, usuario, banco, erro, tamErro);
            m_i->Derrubar();
            return false;
        }

        Handshake h;
        if (!LerHandshake(*m_i, p, h, erro, tamErro)) { m_i->Derrubar(); return false; }
        m_i->versaoServidor = h.versao;
        m_i->capServidor    = h.capacidades;
        m_i->Registrar("MySQL: conectado em %s:%u, versao '%s', metodo pedido '%s'",
                       host, (unsigned)porta, h.versao.c_str(), h.plugin.c_str());

        if (!(h.capacidades & CAP_PROTOCOL_41))
        {
            Diga(erro, tamErro,
                 "o MySQL em '%s:%u' e antigo demais (anterior a 4.1) e nao fala o protocolo "
                 "que este plugin implementa. Versao anunciada: %s.",
                 host, (unsigned)porta, h.versao.c_str());
            m_i->Derrubar();
            return false;
        }

        Metodo metodo = MetodoDoNome(h.plugin);
        if (metodo == Metodo::NENHUM)
        {
            char dica[640];
            DicaDeAutenticacao(h.versao, usuario, dica, (int)sizeof(dica));
            Diga(erro, tamErro,
                 "o MySQL em '%s:%u' quer autenticar o usuario '%s' pelo metodo '%s', que este "
                 "plugin nao implementa (implementados: mysql_native_password, "
                 "caching_sha2_password, sha256_password). %s",
                 host, (unsigned)porta, usuario, h.plugin.c_str(), dica);
            m_i->Derrubar();
            return false;
        }

        // ── 2. resposta ao handshake ────────────────────────────────────
        uint32_t cap = CAP_LONG_PASSWORD | CAP_LONG_FLAG | CAP_PROTOCOL_41
                     | CAP_TRANSACTIONS  | CAP_SECURE_CONNECTION;
        if (h.capacidades & CAP_PLUGIN_AUTH) cap |= CAP_PLUGIN_AUTH;
        if (banco && *banco && (h.capacidades & CAP_CONNECT_WITH_DB))
            cap |= CAP_CONNECT_WITH_DB;
        // Só anunciamos o que o servidor também tem. E jamais LOCAL_FILES nem
        // MULTI_STATEMENTS — ver o comentário do enum de capacidades.
        cap &= (h.capacidades | CAP_PROTOCOL_41 | CAP_SECURE_CONNECTION | CAP_LONG_PASSWORD);
        m_i->capUsadas = cap;

        std::vector<uint8_t> resposta;
        CalcularResposta(metodo, senha, h.sal, resposta);

        std::vector<uint8_t> saida;
        saida.reserve(64 + strlen(usuario) + resposta.size());
        PorLe32(saida, cap);
        PorLe32(saida, 0x01000000u);        // teto de pacote que aceitamos: 16 MiB
        saida.push_back(CHARSET_UTF8MB4);
        for (int i = 0; i < 23; ++i) saida.push_back(0);   // reservado
        saida.insert(saida.end(), usuario, usuario + strlen(usuario));
        saida.push_back(0);
        if (cap & CAP_SECURE_CONNECTION)
        {
            saida.push_back((uint8_t)resposta.size());
            saida.insert(saida.end(), resposta.begin(), resposta.end());
        }
        else
        {
            saida.insert(saida.end(), resposta.begin(), resposta.end());
            saida.push_back(0);
        }
        if (cap & CAP_CONNECT_WITH_DB)
        {
            saida.insert(saida.end(), banco, banco + strlen(banco));
            saida.push_back(0);
        }
        if (cap & CAP_PLUGIN_AUTH)
        {
            const char* nm = NomeMetodo(metodo);
            saida.insert(saida.end(), nm, nm + strlen(nm));
            saida.push_back(0);
        }

        if (!EnviarPacote(*m_i, saida.data(), saida.size(), prazo, erro, tamErro))
        { m_i->Derrubar(); return false; }
        Limpar(saida.data(), saida.size());
        Limpar(resposta.data(), resposta.size());

        // ── 3. o vai-e-vem da autenticação ──────────────────────────────
        uint8_t salAtual[20];
        memcpy(salAtual, h.sal, 20);
        int voltas = 0;
        for (;;)
        {
            if (++voltas > 12)
            {
                // Um servidor forjado pode mandar AuthSwitchRequest para
                // sempre. 12 voltas é muito mais que qualquer fluxo real
                // (o pior caso real são 4).
                Diga(erro, tamErro,
                     "o MySQL em '%s:%u' ficou trocando de metodo de autenticacao sem parar. "
                     "Desisti para nao ficar preso.", host, (unsigned)porta);
                m_i->Derrubar();
                return false;
            }
            if (!LerPacote(*m_i, p, prazo, erro, tamErro)) { m_i->Derrubar(); return false; }
            if (p.empty())
            {
                Diga(erro, tamErro, "o MySQL mandou um pacote vazio durante a autenticacao.");
                m_i->Derrubar();
                return false;
            }

            if (p[0] == 0x00) break;                     // OK: autenticado

            if (p[0] == 0xFF)
            {
                unsigned cod = 0; std::string est, msg;
                LerErr(p, cod, est, msg);
                m_i->codigoMysql = cod;
                ExplicarErroDoServidor(cod, msg, usuario, banco, erro, tamErro);
                m_i->Derrubar();
                return false;
            }

            if (p[0] == 0xFE)
            {
                if (p.size() == 1)
                {
                    char dica[640];
                    DicaDeAutenticacao(h.versao, usuario, dica, (int)sizeof(dica));
                    Diga(erro, tamErro,
                         "o MySQL em '%s:%u' pediu o metodo de senha anterior ao 4.1 "
                         "(mysql_old_password), que e' inseguro e nao esta implementado aqui. %s",
                         host, (unsigned)porta, dica);
                    m_i->Derrubar();
                    return false;
                }
                // AuthSwitchRequest: 0xFE + nome do plugin + sal novo
                Cursor c(p.data(), p.size());
                uint8_t b = 0; c.U8(b);
                std::string novoPlugin;
                if (!c.StrNul(novoPlugin))
                {
                    Diga(erro, tamErro, "o MySQL mandou um pedido de troca de metodo "
                                        "malformado durante a autenticacao.");
                    m_i->Derrubar();
                    return false;
                }
                const uint8_t* q = nullptr; size_t nq = 0;
                c.Resto(&q, nq);
                if (nq >= 20) memcpy(salAtual, q, 20);

                Metodo novo = MetodoDoNome(novoPlugin);
                if (novo == Metodo::NENHUM)
                {
                    char dica[640];
                    DicaDeAutenticacao(h.versao, usuario, dica, (int)sizeof(dica));
                    Diga(erro, tamErro,
                         "o MySQL em '%s:%u' quer trocar para o metodo de autenticacao '%s', que "
                         "este plugin nao implementa. %s",
                         host, (unsigned)porta, novoPlugin.c_str(), dica);
                    m_i->Derrubar();
                    return false;
                }
                metodo = novo;
                m_i->Registrar("MySQL: servidor pediu troca para '%s'", novoPlugin.c_str());
                std::vector<uint8_t> r2;
                CalcularResposta(metodo, senha, salAtual, r2);
                if (!EnviarPacote(*m_i, r2.data(), r2.size(), prazo, erro, tamErro))
                { m_i->Derrubar(); return false; }
                Limpar(r2.data(), r2.size());
                continue;
            }

            if (p[0] == 0x01)
            {
                // AuthMoreData
                if (metodo == Metodo::CACHING_SHA2)
                {
                    if (p.size() < 2)
                    {
                        Diga(erro, tamErro, "o MySQL mandou um pacote de autenticacao "
                                            "caching_sha2_password vazio.");
                        m_i->Derrubar(); return false;
                    }
                    if (p[1] == 0x03) continue;          // senha já estava no cache: segue
                    if (p[1] != 0x04)
                    {
                        Diga(erro, tamErro,
                             "o MySQL mandou um codigo de autenticacao caching_sha2_password "
                             "que eu nao conheco (0x%02X).", (unsigned)p[1]);
                        m_i->Derrubar(); return false;
                    }
                    // 0x04 = autenticação completa. Sem TLS, o caminho é
                    // pedir a chave pública e mandar a senha cifrada.
                    if (!senha || !*senha)
                    {
                        Diga(erro, tamErro,
                             "o MySQL exige autenticacao completa para o usuario '%s', mas "
                             "MysqlPass esta vazio no config.json.", usuario);
                        m_i->Derrubar(); return false;
                    }
                    const uint8_t pedido[1] = { 0x02 };   // REQUEST_PUBLIC_KEY
                    if (!EnviarPacote(*m_i, pedido, 1, prazo, erro, tamErro))
                    { m_i->Derrubar(); return false; }
                    if (!LerPacote(*m_i, p, prazo, erro, tamErro))
                    { m_i->Derrubar(); return false; }
                    if (p.empty() || p[0] != 0x01)
                    {
                        if (EhErr(p))
                        {
                            unsigned cod = 0; std::string est, msg;
                            LerErr(p, cod, est, msg);
                            m_i->codigoMysql = cod;
                            char dica[640];
                            DicaDeAutenticacao(h.versao, usuario, dica, (int)sizeof(dica));
                            Diga(erro, tamErro,
                                 "o MySQL exige o metodo caching_sha2_password e recusou "
                                 "entregar a chave publica necessaria para completa-lo (erro "
                                 "%u: %s). Isso costuma acontecer quando o servidor foi subido "
                                 "com caching_sha2_password_auto_generate_rsa_keys=OFF. %s",
                                 cod, msg.c_str(), dica);
                        }
                        else
                        {
                            Diga(erro, tamErro,
                                 "o MySQL nao respondeu a chave publica que eu pedi para "
                                 "completar o metodo caching_sha2_password.");
                        }
                        m_i->Derrubar(); return false;
                    }

                    Num mod, expo; size_t k = 0;
                    if (!RsaDoPem((const char*)p.data() + 1, p.size() - 1, mod, expo, k,
                                  erro, tamErro))
                    { m_i->Derrubar(); return false; }

                    // senha + '\0', XOR com o sal repetido — exatamente o que
                    // o servidor desfaz do outro lado
                    size_t tamSenha = strlen(senha) + 1;
                    std::vector<uint8_t> claro(tamSenha);
                    memcpy(claro.data(), senha, tamSenha);
                    for (size_t i = 0; i < tamSenha; ++i) claro[i] ^= salAtual[i % 20];

                    int64_t t0 = AgoraMs();
                    std::vector<uint8_t> cifrada;
                    const char* fonteAleatoria = "(nenhum)";
                    bool okRsa = RsaCifrarOaep(mod, expo, k, claro.data(), claro.size(),
                                               cifrada, erro, tamErro, &fonteAleatoria);
                    Limpar(claro.data(), claro.size());
                    if (!okRsa) { m_i->Derrubar(); return false; }
                    m_i->Registrar("MySQL: autenticacao completa (RSA de %d bits, aleatorio de "
                                   "%s) em %d ms",
                                   (int)(k * 8), fonteAleatoria, (int)(AgoraMs() - t0));

                    if (!EnviarPacote(*m_i, cifrada.data(), cifrada.size(), prazo, erro, tamErro))
                    { m_i->Derrubar(); return false; }
                    continue;
                }
                if (metodo == Metodo::SHA256)
                {
                    // sha256_password sem TLS: o servidor manda a chave
                    // pública em resposta ao nosso 0x01.
                    Num mod, expo; size_t k = 0;
                    if (!RsaDoPem((const char*)p.data() + 1, p.size() - 1, mod, expo, k,
                                  erro, tamErro))
                    { m_i->Derrubar(); return false; }
                    size_t tamSenha = strlen(senha ? senha : "") + 1;
                    std::vector<uint8_t> claro(tamSenha);
                    memcpy(claro.data(), senha ? senha : "", tamSenha);
                    for (size_t i = 0; i < tamSenha; ++i) claro[i] ^= salAtual[i % 20];
                    std::vector<uint8_t> cifrada;
                    bool okRsa = RsaCifrarOaep(mod, expo, k, claro.data(), claro.size(),
                                               cifrada, erro, tamErro);
                    Limpar(claro.data(), claro.size());
                    if (!okRsa) { m_i->Derrubar(); return false; }
                    if (!EnviarPacote(*m_i, cifrada.data(), cifrada.size(), prazo, erro, tamErro))
                    { m_i->Derrubar(); return false; }
                    continue;
                }
                Diga(erro, tamErro, "o MySQL mandou dados extras de autenticacao para o metodo "
                                    "'%s', que nao os usa.", NomeMetodo(metodo));
                m_i->Derrubar(); return false;
            }

            Diga(erro, tamErro,
                 "o MySQL mandou um pacote inesperado (primeiro byte 0x%02X) durante a "
                 "autenticacao.", (unsigned)p[0]);
            m_i->Derrubar();
            return false;
        }

        // ── 4. endurecimento pós-login (INV-MYSQL-004) ──────────────────
        //
        // A conexão está aberta, mas ainda NÃO está apta a receber SQL
        // interpolado. Duas condições do servidor tornam Escapar() inseguro,
        // as duas silenciosas, e as duas são conferidas aqui.

        // (a) conjunto de caracteres. Em GBK/SJIS/BIG5 o byte 0x5C ('\') pode
        //     ser a segunda metade de um caractere, e a barra que eu ponho na
        //     frente da aspa é engolida pelo caractere anterior — a aspa
        //     escapa e a injeção passa. Em utf8mb4 isso é impossível.
        if (!ComandoSimples(*m_i, "SET NAMES utf8mb4", erro, tamErro))
        {
            Diga(erro, tamErro,
                 "conectei no MySQL em '%s:%u' mas nao consegui mudar a conexao para utf8mb4, e "
                 "sem isso a protecao contra injecao de SQL nao vale. Derrubei a conexao. "
                 "Confira se o MySQL tem o conjunto de caracteres utf8mb4 (existe desde a "
                 "versao 5.5).", host, (unsigned)porta);
            m_i->Derrubar();
            return false;
        }

        // (b) NO_BACKSLASH_ESCAPES. Com esse modo ligado, o servidor NÃO
        //     entende '\' como escape, e o meu \' vira uma barra seguida de
        //     uma aspa que FECHA o literal. Todo Escapar() deste arquivo
        //     passaria a produzir SQL injetável, sem erro nenhum.
        //     Tento tirar do modo só desta sessão; se não sair, a conexão
        //     morre aqui. Falha fechada.
        {
            std::string modo; bool achou = false;
            if (!ConsultaUmValor(*m_i, "SELECT @@SESSION.sql_mode", modo, achou, erro, tamErro))
            { m_i->Derrubar(); return false; }
            if (achou && modo.find("NO_BACKSLASH_ESCAPES") != std::string::npos)
            {
                m_i->Registrar("MySQL: sql_mode tem NO_BACKSLASH_ESCAPES; tentando remover "
                               "so nesta sessao");
                if (!ComandoSimples(*m_i,
                        "SET SESSION sql_mode = REPLACE(REPLACE(@@SESSION.sql_mode,"
                        "'NO_BACKSLASH_ESCAPES',''),',,',',')", erro, tamErro))
                { m_i->Derrubar(); return false; }

                modo.clear(); achou = false;
                if (!ConsultaUmValor(*m_i, "SELECT @@SESSION.sql_mode", modo, achou, erro, tamErro))
                { m_i->Derrubar(); return false; }
                if (!achou || modo.find("NO_BACKSLASH_ESCAPES") != std::string::npos)
                {
                    Diga(erro, tamErro,
                         "o MySQL em '%s:%u' esta com NO_BACKSLASH_ESCAPES no sql_mode e nao "
                         "deixou remover na sessao. Com esse modo ligado, a protecao contra "
                         "injecao de SQL deste plugin NAO funciona, entao me recusei a "
                         "conectar. Tire NO_BACKSLASH_ESCAPES do sql_mode no my.cnf do MySQL.",
                         host, (unsigned)porta);
                    m_i->Derrubar();
                    return false;
                }
            }
        }

        m_i->Registrar("MySQL: pronto (%s, utf8mb4, sem NO_BACKSLASH_ESCAPES)",
                       m_i->versaoServidor.c_str());
        return true;
    }
    catch (const std::bad_alloc&)
    {
        m_i->Derrubar();
        Diga(erro, tamErro, "faltou memoria ao conectar no MySQL.");
        return false;
    }
    catch (...)
    {
        // INV-MYSQL-001: nada atravessa a fronteira desta classe.
        m_i->Derrubar();
        Diga(erro, tamErro, "erro interno inesperado ao conectar no MySQL.");
        return false;
    }
}


// ============================================================================
//  §7 · COMANDOS
// ============================================================================
namespace
{

// Núcleo compartilhado por Executar e Consultar. 'linha' pode ser nulo — aí
// as linhas são lidas e descartadas (mas SÃO lidas: ver INV-MYSQL-003).
bool Comando(MySqlInterno& I, const char* sql,
             void (*linha)(void*, int, const char* const*), void* ctx,
             char* erro, int tamErro)
{
    I.codigoMysql = 0;
    I.linhasAfetadas = 0;
    I.ultimoId = 0;
    I.colunas.clear();
    I.comprimentos.clear();

    // Devolve a memoria de uma resposta grande ANTERIOR.
    //
    // O buffer e reaproveitado de proposito (uma alocacao por conexao, nao uma
    // por consulta), mas std::vector nunca encolhe sozinho: uma unica consulta
    // que trouxesse 50 MB deixaria esses 50 MB presos pelo resto da vida do
    // processo do jogo. Ninguem liga isso ao banco depois — vira "o servidor
    // esta comendo memoria" e a caca comeca no lugar errado. 1 MiB cobre com
    // folga qualquer resposta desta aplicacao; acima disso a memoria volta.
    if (I.pacote.capacity() > 1024u * 1024u)
        std::vector<uint8_t>().swap(I.pacote);

    if (I.soq == SOQ_NULO || I.quebrado)
    {
        Diga(erro, tamErro,
             "nao ha conexao aberta com o MySQL (ou ela caiu). Reconecte antes de executar "
             "SQL.");
        return false;
    }
    if (!sql)
    {
        Diga(erro, tamErro, "SQL nulo.");
        return false;
    }

    int64_t prazo = AgoraMs() + I.msOperar;
    size_t nSql = strlen(sql);

    // COM_QUERY = 0x03. Cada comando começa uma sequência nova em 0.
    I.seq = 0;
    std::vector<uint8_t> req;
    req.reserve(nSql + 1);
    req.push_back(0x03);
    req.insert(req.end(), sql, sql + nSql);
    if (!EnviarPacote(I, req.data(), req.size(), prazo, erro, tamErro)) return false;

    std::vector<uint8_t>& p = I.pacote;
    if (!LerPacote(I, p, prazo, erro, tamErro)) return false;
    if (p.empty())
    {
        I.quebrado = true;
        Diga(erro, tamErro, "o MySQL respondeu um pacote vazio a uma consulta.");
        return false;
    }

    if (p[0] == 0xFF)
    {
        unsigned cod = 0; std::string est, msg;
        LerErr(p, cod, est, msg);
        I.codigoMysql = cod;
        // Erro do servidor NÃO quebra a conexão: ela continua utilizável.
        Diga(erro, tamErro, "o MySQL recusou o comando (erro %u%s%s): %s",
             cod, est.empty() ? "" : ", SQLSTATE ", est.c_str(), msg.c_str());
        return false;
    }
    if (p[0] == 0x00 || (p[0] == 0xFE && p.size() < 9))
    {
        LerOk(p, I.linhasAfetadas, I.ultimoId);
        return true;                                  // sem resultado
    }
    if (p[0] == 0xFB)
    {
        // LOCAL INFILE. Nunca pedimos CLIENT_LOCAL_FILES, então um servidor
        // que manda isso está tentando fazer este processo ler arquivos do
        // disco e entregá-los — o ataque conhecido do "rogue MySQL server".
        I.quebrado = true;
        Diga(erro, tamErro,
             "o servidor em '%s:%u' pediu para eu ENVIAR UM ARQUIVO do disco desta maquina "
             "(LOCAL INFILE). Eu nunca ofereci esse recurso, entao ou nao e' um MySQL de "
             "verdade, ou alguem esta no meio da conexao. Derrubei a conexao e nao mandei "
             "nada.", I.hostAtual.c_str(), (unsigned)I.portaAtual);
        return false;
    }

    // Cabeçalho de resultado: número de colunas (inteiro variável)
    uint64_t nCols = 0; bool nulo = false;
    {
        Cursor c(p.data(), p.size());
        if (!c.VarInt(nCols, nulo) || nulo || nCols == 0 || nCols > 4096)
        {
            I.quebrado = true;
            Diga(erro, tamErro,
                 "o MySQL respondeu um cabecalho de resultado que eu nao entendi (%s "
                 "colunas). Derrubei a conexao para nao ler dado trocado.",
                 Dec(nCols).c_str());
            return false;
        }
    }

    // Definição das colunas
    I.colunas.resize((size_t)nCols);
    for (uint64_t i = 0; i < nCols; ++i)
    {
        if (!LerPacote(I, p, prazo, erro, tamErro)) return false;
        Cursor c(p.data(), p.size());
        const uint8_t* q = nullptr; size_t n = 0; bool nl = false;
        // catalog, schema, table, org_table, name — todos com comprimento
        // variável na frente. Só o `name` interessa.
        if (!c.VarStr(&q, n, nl) || !c.VarStr(&q, n, nl) || !c.VarStr(&q, n, nl)
            || !c.VarStr(&q, n, nl) || !c.VarStr(&q, n, nl))
        {
            I.quebrado = true;
            Diga(erro, tamErro, "a definicao da coluna %d veio truncada do MySQL.", (int)i);
            return false;
        }
        I.colunas[(size_t)i].assign(q ? (const char*)q : "", n);
    }

    // Com CLIENT_DEPRECATE_EOF desligado (é o nosso caso), vem um EOF aqui
    // separando colunas de linhas. Servidor que não mande é servidor que já
    // está mandando a primeira linha — os dois casos são tratados.
    if (!LerPacote(I, p, prazo, erro, tamErro)) return false;
    bool primeiraJaLida = !EhEof(p);

    I.celulas.assign((size_t)nCols, std::string());
    I.ponteiros.assign((size_t)nCols, nullptr);
    I.comprimentos.assign((size_t)nCols, 0);

    for (;;)
    {
        if (!primeiraJaLida)
        {
            if (!LerPacote(I, p, prazo, erro, tamErro)) return false;
        }
        primeiraJaLida = false;

        if (p.empty())
        {
            I.quebrado = true;
            Diga(erro, tamErro, "o MySQL mandou um pacote vazio no meio do resultado.");
            return false;
        }
        if (p[0] == 0xFF)
        {
            // O MySQL pode abortar um resultado no meio (por exemplo, erro de
            // conversão numa linha). O fluxo continua íntegro.
            unsigned cod = 0; std::string est, msg;
            LerErr(p, cod, est, msg);
            I.codigoMysql = cod;
            Diga(erro, tamErro, "o MySQL interrompeu o resultado com erro %u: %s",
                 cod, msg.c_str());
            return false;
        }
        if (EhEof(p)) break;                          // fim das linhas
        if (p[0] == 0x00 && p.size() >= 7 && (I.capUsadas & CAP_DEPRECATE_EOF)) break;

        Cursor c(p.data(), p.size());
        for (uint64_t i = 0; i < nCols; ++i)
        {
            const uint8_t* q = nullptr; size_t n = 0; bool nl = false;
            if (!c.VarStr(&q, n, nl))
            {
                I.quebrado = true;
                Diga(erro, tamErro,
                     "uma linha do resultado veio truncada (coluna %d de %d). Derrubei a "
                     "conexao.", (int)i + 1, (int)nCols);
                return false;
            }
            if (nl)
            {
                I.ponteiros[(size_t)i]    = nullptr;   // NULL do SQL
                I.comprimentos[(size_t)i] = 0;
                I.celulas[(size_t)i].clear();
            }
            else
            {
                I.celulas[(size_t)i].assign(q ? (const char*)q : "", n);
                I.ponteiros[(size_t)i]    = I.celulas[(size_t)i].c_str();
                I.comprimentos[(size_t)i] = n;
            }
        }
        ++I.linhasAfetadas;                            // aqui = linhas lidas

        if (linha)
        {
            try
            {
                linha(ctx, (int)nCols, I.ponteiros.data());
            }
            catch (...)
            {
                // INV-MYSQL-001 e INV-MYSQL-003: a exceção do chamador não
                // atravessa, e o resto do resultado ficou no fio — a conexão
                // não serve mais.
                I.quebrado = true;
                Diga(erro, tamErro,
                     "a funcao que recebe as linhas lancou uma excecao. Abortei a consulta e "
                     "derrubei a conexao (o resto do resultado ficaria no meio do caminho).");
                return false;
            }
        }
    }
    return true;
}

bool ComandoSimples(MySqlInterno& I, const char* sql, char* erro, int tamErro)
{
    return Comando(I, sql, nullptr, nullptr, erro, tamErro);
}

struct UmValor { std::string v; bool achou; };

void PegarUmValor(void* ctx, int ncols, const char* const* valores)
{
    UmValor* u = (UmValor*)ctx;
    if (u->achou || ncols < 1) return;
    u->achou = true;
    u->v = valores[0] ? valores[0] : "";
}

bool ConsultaUmValor(MySqlInterno& I, const char* sql, std::string& valor,
                     bool& achou, char* erro, int tamErro)
{
    UmValor u; u.achou = false;
    if (!Comando(I, sql, PegarUmValor, &u, erro, tamErro)) return false;
    valor = u.v;
    achou = u.achou;
    return true;
}

} // namespace anônimo


bool MySqlCliente::Executar(const char* sql, char* erro, int tamErro)
{
    if (erro && tamErro > 0) erro[0] = '\0';
    Reserva r(m_i);
    if (!r.Ok())
    {
        Diga(erro, tamErro,
             "esta conexao com o MySQL ja esta sendo usada por outra thread. Cada thread "
             "precisa da sua propria conexao.");
        return false;
    }
    if (m_i->interrompido.load(std::memory_order_acquire))
    {
        Diga(erro, tamErro, "o servidor esta desligando; este comando nao foi enviado ao MySQL.");
        return false;
    }
    try
    {
        // 'linha' nulo: se o SQL devolver linhas por engano, elas são lidas e
        // jogadas fora. Deixar de ler é que dessincroniza o fluxo.
        return Comando(*m_i, sql, nullptr, nullptr, erro, tamErro);
    }
    catch (const std::bad_alloc&)
    {
        m_i->quebrado = true;
        Diga(erro, tamErro, "faltou memoria executando SQL no MySQL.");
        return false;
    }
    catch (...)
    {
        m_i->quebrado = true;
        Diga(erro, tamErro, "erro interno inesperado executando SQL no MySQL.");
        return false;
    }
}

bool MySqlCliente::Consultar(const char* sql,
                             void (*linha)(void*, int, const char* const*),
                             void* ctx, char* erro, int tamErro)
{
    if (erro && tamErro > 0) erro[0] = '\0';
    Reserva r(m_i);
    if (!r.Ok())
    {
        Diga(erro, tamErro,
             "esta conexao com o MySQL ja esta sendo usada por outra thread. Cada thread "
             "precisa da sua propria conexao.");
        return false;
    }
    if (m_i->interrompido.load(std::memory_order_acquire))
    {
        Diga(erro, tamErro, "o servidor esta desligando; esta consulta nao foi enviada ao MySQL.");
        return false;
    }
    try
    {
        return Comando(*m_i, sql, linha, ctx, erro, tamErro);
    }
    catch (const std::bad_alloc&)
    {
        m_i->quebrado = true;
        Diga(erro, tamErro, "faltou memoria lendo resultado do MySQL.");
        return false;
    }
    catch (...)
    {
        m_i->quebrado = true;
        Diga(erro, tamErro, "erro interno inesperado lendo resultado do MySQL.");
        return false;
    }
}

uint64_t    MySqlCliente::LinhasAfetadas()    const { return m_i->linhasAfetadas; }
uint64_t    MySqlCliente::UltimoIdInserido()  const { return m_i->ultimoId; }
unsigned    MySqlCliente::UltimoCodigoMysql() const { return m_i->codigoMysql; }
const char* MySqlCliente::VersaoServidor()    const { return m_i->versaoServidor.c_str(); }

const std::vector<std::string>& MySqlCliente::Colunas() const { return m_i->colunas; }
const std::vector<size_t>& MySqlCliente::ComprimentosDaLinha() const { return m_i->comprimentos; }

} // namespace Perm
