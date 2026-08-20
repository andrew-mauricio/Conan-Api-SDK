# Conan Shop

*Portuguese translation: [README-CONANSHOP.pt.md](README-CONANSHOP.pt.md)*

A points shop for Conan Exiles Enhanced. Players earn points for time online,
see the list with `!shop` and buy with `!comprar`. VIPs earn more, and what
decides that is your `permission.json`.

It's the same design as **ArkShop**, which plenty of people already know from
ARK and ASA — adapted to Conan, which works differently underneath (see
[An item is not a blueprint](#an-item-is-not-a-blueprint)).

---

## Installing

Copy the whole `ConanShop` folder into:

```
<server>/ConanSandbox/Binaries/Win64/Conan-Api/Plugins/
```

The folder holds four things, and three of them ship in the package:

```
Conan-Api/Plugins/ConanShop/
   ConanShop.dll        the plugin
   PluginInfo.json      what the API reads to know what it is
   config.json          what YOU edit
   conanshop.db         born on its own the first time (the points database)
```

**Permission is required.** It's where the player's identity comes from (the
wallet's key) and where VIP status comes from. Without it the plugin loads,
says so in the log, and can't identify anybody.

That's it. Start the server and the shop is live.

---

## The commands

| in chat | who can | what it does |
|---|---|---|
| `!shop` | everyone | shows the list on screen |
| `!shop 2` | everyone | page 2 |
| `!comprar pedra` | everyone | buys and delivers to the inventory |
| `!comprar pedra 3` | everyone | buys three times |
| `!pontos` | everyone | shows the balance |
| `!shopajuda` | everyone | reminds you of the commands |
| `!shopdar Fulano#123 500` | admin | gives somebody points |
| `!shopreload` | admin | re-reads `config.json` |

All of them can be renamed in `config.json`, under `"comandos"`.

### The `!shop` screen

The list appears in the game's message box, the one with a button underneath.

**The button belongs to the game and only closes.** There's no turning it into
"next page": it isn't ours. Paging is by command: `!shop 2`, `!shop 3`. The
list's footer reminds the player of that.

If you'd rather have the list in chat instead of on screen, set
`"usar_tela": false`.

---

## Giving points from outside the game

A web panel, a script, a scheduled task, SSH. Write lines into:

```
<server>/ConanSandbox/Binaries/Win64/Conan-Api/SHOP-COMANDOS
```

```
dar     Indio#76973 500 premio do evento
tirar   A-4QR7CRS0F 100 estorno
definir Indio#76973 0
saldo   Indio#76973
recarregar
```

The plugin picks them up within 3 seconds and answers, line by line, in
`Conan-Api/SHOP-RESPOSTAS`:

```
# ConanShop — respostas de 2026-08-20 04:15:02
linha 1: ok dar A-4QR7CRS0F +500 (saldo 1250)
linha 2: RECUSADO tirar A-9XX -100 — saldo insuficiente (tem 40). NADA foi tirado.
```

The player can be identified by **display name** (which only works while they're
online — the answer says so) or by **account id**, which always works.

### Why this isn't an RCON command

Because it can't be, and that was **measured**, not assumed:

- Conan's RCON takes a fixed list of commands (`help` shows: `listplayers`,
  `broadcast`, `con`, `exec`, `sql`, `KickPlayer`…). There's no registering a new
  one — AsaApi has `AddRconCommand`, Conan exposes no equivalent.
- The next hope was to hitch a ride: `broadcast <text>` calls
  `ConanCheatManager::BroadcastMessage`, which **is** a UFunction and so ought to
  be hookable. It was tested with five hooks armed at once. The server answered
  *"Message has been broadcast."* and **none of them fired**. RCON's path is
  native and doesn't go through the game's reflection.
- And `exec` with no argument **takes the server down** (an access violation — a
  Conan bug, not the plugin's). Don't send it.

The file is the door that was left, and it's the one every automation already
knows how to open.

---

## The `config.json`

The file is commented from the inside (the `_leia_isto` keys). The short
version:

### Points

```json
"pontos": {
  "ligado": true,
  "minutos": 5,
  "somar": false,
  "grupos": { "default": 5, "vip": 15, "vipplus": 25 }
}
```

The names under `grupos` are the **group keys from your `permission.json`**.

`"somar": false` (the default) gives the player the **highest** value among
their groups — a VIP who is also in `default` earns 15, not 20. `true` adds them
up.

With the defaults (5 points every 5 minutes), an ordinary player makes **60
points an hour**. That's the ruler for reading the prices.

### Database

`"tipo": "local"` keeps it in a file beside the plugin. Nothing to install, and
it's the right answer for almost everybody.

`"tipo": "mysql"` is only for people running **several servers** who want points
to count on all of them. Then `mysql_usuario` and `mysql_banco` become
mandatory — and if they're empty the plugin **won't come up**, on purpose:
quietly falling back to local would send your players' points into a file you're
never going to look at.

> Deleting the local database means deleting `conanshop.db-wal` and
> `conanshop.db-shm` too. Without that the data **comes back** on the next open
> and the deletion only looked like it happened.

### Items

```json
"pedra": {
  "nome": "Pedra",
  "categoria": "recurso",
  "template_id": 10001,
  "quantidade": 100,
  "preco": 10
}
```

The key (`pedra`) is what the player types: `!comprar pedra`.

`"permissao": "shop.vip"` (optional) restricts the item — and it doesn't even
appear in the list for somebody who can't buy it, rather than appearing and
being refused at purchase time.

---

## An item is not a blueprint

This is the difference that gives ARK people the most trouble.

In ARK *"one blueprint = one item"* holds, and the shop stores the blueprint's
path. **In Conan it doesn't.** What identifies an item is the **Template ID** —
the *Row Name* of the `/Game/Items/ItemTable` table, which is Funcom's canonical
table.

That row's `ItemClass` field points at the blueprint, but it is **not unique**:

| item | ID | ItemClass |
|---|---|---|
| Stone | 10001 | `/Script/ConanSandbox.GameItem` ← native class, shared |
| Brimstone | 14171 | `/Script/ConanSandbox.GameItem` ← **the same one** |
| Katana | 51091 | `/Game/Items/Weapons/Katana2h/BP_Item_KatanaBase…` |

Hundreds of simple items share that same native class. A shop modelled on
blueprints would deliver the wrong item for that whole family — and it would
**work**, without a single error in the log.

That's why `config.json` always sells by `template_id`.

---

## Where the item list comes from

From the **ExtratorItemTable** plugin, which ships with Conan-Api. With the
server already loaded, create the file:

```
<server>/ConanSandbox/Binaries/Win64/Conan-Api/EXTRAIR-ITEMTABLE
```

It reads all of `/Game/Items/ItemTable` and writes, beside the plugin:

```
itemtable-conan.json     for a program
itemtable-conan.csv      for a spreadsheet (with a BOM, so Excel gets the accents right on its own)
```

On this build (24784646) that's **9,121 items × 120 columns**, with `Name`,
`GUICategory`, `ItemClass`, `ItemTier`, `DLCPackage`, `MaxStackSize` and the
rest.

It does **not** depend on somebody having opened a chest: it reads the table, not
the loaded objects. An extractor that sweeps the world returns a short list that
*looks* complete — which is what happened on this project's first attempt: 27
items, all with Template ID zero, in a world of 786,927 objects.

To turn that into a shop catalogue:

```bash
tools/montar_catalogo_loja.py itemtable-conan.json --max-por-categoria 12 -o itens.json
```

The script **says what it left out and why** (DLC items, decoration, test rows),
and the arithmetic adds up to the total — a silent cut turns into "the shop is
incomplete and I don't know why".

---

## What this plugin protects, and how

### Nobody spends what they don't have, or spends it twice

The debit is **a single UPDATE with the balance condition inside it**:

```sql
UPDATE carteira SET pontos = pontos - ? WHERE jogador = ? AND pontos >= ?
```

The database decides, once, under its own lock. There's no gap between "read the
balance" and "spend it", because nothing is read before spending. If the row
didn't change, there was no balance — and that's an **answer from the database**,
not an assumption.

This is measured. Eight simultaneous clients, 400 purchase attempts fighting over
a balance of exactly 20:

| implementation | went through | final balance |
|---|---|---|
| check outside the UPDATE (ArkShop's pattern) | **26** | **−60** |
| condition inside the UPDATE (this plugin) | **20** | **0** |

The test runs with `testes/rodar.sh`, and there's a `controle_positivo` that
proves the test **knows how to fail**. That isn't ceremony: the first version of
the test passed, and the positive control failed it — there was a mutex inside
the test itself, and it erased exactly the gap we were trying to measure. With
it there, even the broken implementation passed with full marks. Until the
positive control passes, the rest of the suite means nothing.

This matters for real when you point two servers at the same MySQL: there's no
lock shared between the processes there, and only the SQL holds.

---

### No money command comes out truncated

All SQL goes through `MontarSql`, which **refuses the operation** if the command
doesn't fit the buffer, instead of quietly truncating the way `snprintf` does.

The reason is specific: a cut in the wrong place produces a command that is
*valid* and *different from what was meant*. Cutting between
`WHERE jogador='...'` and `AND pontos >= N` leaves an UPDATE the database is
happy to run — **without the balance condition**. On a credit it's worse: a cut
before the `WHERE` credits everybody's wallet.

With account ids capped at 64 characters that can't happen today. But "it can't
happen today" was never a guarantee, and the protection costs one line.

### Broken configuration doesn't replace good configuration

You edit `config.json` at 9pm, miss a comma and send `!shopreload`. The plugin
**refuses** and carries on with the previous configuration, saying why in chat.

Without that, the shop would sit empty through peak hours — with no visible
error, because "zero items" is a valid state.

### If delivery fails, the points come back

The debit happens before delivery (it's the only safe order). If the game refuses
the item, the plugin **refunds** and records why in the ledger and in the log.

### Every movement is in the ledger

The `diario` table keeps every credit and debit with a date, an amount and a
reason. When a player says *"I bought it and didn't get it"*, that's what
answers.

---

## What this plugin does NOT solve

Stated on purpose, because a known limit beats a surprise.

### The server dying between the debit and the delivery

The order is: debit → deliver → refund if delivery fails. It's the only safe
order (delivering before charging would hand out a free item if the debit
failed).

But if the **server dies exactly in between** — after the debit, before the
delivery — the points go and the item doesn't arrive, and nothing is running to
refund it.

**This has not happened in production.** The shop has been running with real
players buying: no crashes, no lost points, no wrong balances. The window is
described here because it exists in the code, not because anybody has hit it.

Solving it properly would mean marking the purchase pending and reconciling on
the next startup; that's a state machine that isn't here. What does exist:

- the window is milliseconds wide (between two consecutive calls);
- the `diario` records the debit as `compra:<item>`, so you can see what happened
  and refund by hand:
  ```
  dar Fulano#1234 50 estorno da queda de 20/08
  ```

ArkShop doesn't solve this either.

### Partial delivery when the inventory fills mid-way

`SpawnTemplateItem` takes the whole quantity and answers yes or no. If the game
delivers part of it and refuses the rest, the plugin sees "yes" and charges for
everything. There's no telling them apart from the return value.

Mitigation: items are sold in quantities that respect the table's
`MaxStackSize`, so the stack fits one slot. Even so, buying with a nearly full
inventory can yield less than you paid for.

### DLC items — not tested

Delivery is proved for **vanilla items**: they all arrive. DLC items have not
been tested, and until they are, this stays in the "not proved" column rather
than the "works" one.

The generated catalogue excludes them by default (`DLCPackage != None`) for a
separate reason: somebody who doesn't own the DLC would buy and receive
nothing. If you enable them with `--com-dlc`, that responsibility becomes yours
— the plugin has no way of knowing which DLCs each player owns.

---

## What is proved, and how

This project doesn't call something done that nobody has seen working.
Everything below was measured; none of it is "it should work".

| piece | state |
|---|---|
| wallet, atomic debit, refund | **proved** — automated test with a positive control |
| reading and refusing `config.json` | **proved** — 8 cases, including valid JSON holding invalid items |
| command routing (`!shop` vs `!shopreload`) | **proved** — 11 routing cases |
| ItemTable extraction (9,121 items) | **proved** on the server, IDs checked against an external source |
| the `SHOP-COMANDOS` queue | **proved** — 10 cases, including the ones that must fail |
| RCON does **not** intercept | **proved** — 5 hooks armed, none fired |
| `!pontos` answering | **proved with a real player** (2026-08-20) |
| `!shop` drawing the screen | **proved with a real player** |
| `!comprar` delivering the item | **proved with a real player** — every vanilla item delivers; the first was 100 stone in the inventory |
| credit for time online | **proved with a real player** — credited during the test |

### What the test with a real player found, and no automated test would

The first version **answered nothing** to the player. The chat hook fired, read
the command and cancelled the message correctly — and the reply died on the way,
without a line in the log.

Two causes, both invisible:

1. `Falar()` gave up silently when the player's name hadn't been read — and
   `Identificar()` returns success with an empty name, as long as Permission
   resolved the id.
2. Worse, it was a design mistake: the reply looked the player up **by name**,
   through `CheatManager`, with their controller already in hand.

Today the reply goes straight to the controller, trying three paths in order
(`ClientHUDShowNotification` → `ClientMessage` → `PlayerMessage`) and recording
which one worked. On build 24784646 it's the first, the same one the game uses
for its own notices.

*A 200 from curl doesn't prove the screen.* This defect went through the test
suite, an adversarial review and a good-faith review without showing up. It only
showed up when somebody typed `!pontos` inside the game.

---

## Licence

Same as Conan-Api's. You can run it on as many servers as you like, including
servers that charge their players; and you can write and **sell** plugins of your
own on top of the API. What you can't do is resell or re-host the API itself.
