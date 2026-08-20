# Publishing a plugin — what we ask, and why

*Portuguese translation: [PUBLICAR-PLUGIN.pt.md](PUBLICAR-PLUGIN.pt.md)*

There's no paperwork here. But there are three things we ask, and each one came
out of a real problem.

## 1. Ship the source alongside

Your plugin runs **inside somebody else's server process**, with the same
powers the server has: it reaches all of memory, the players' identity data,
and any file the server can touch.

There's no sandbox. There won't be one — that's what "native plugin" means in
any game, and pretending otherwise would be worse than saying it plainly.

So the only real protection a server owner has is **being able to read what
they're about to install**, or trusting whoever wrote it. Every example in this
SDK ships with source, and we expect the same from anyone publishing.

None of that stops you charging for your plugin. It stops you asking strangers
to run code nobody can see.

## 2. Test on a real server first

Compiling isn't working. This project learned that the expensive way, several
times over:

- a test printed `CORRECT` for a wrong call, because every value in it was
  positive and the error cancelled itself out;
- a hook registered under a name that didn't exist came up fine, never fired,
  and nobody found out — the log said everything was okay;
- a plugin wrote 9 MB per boot into the wrong folder, and it only surfaced when
  somebody went looking for why.

Bring up a local server, log in with a character, and use the feature. No unit
test substitutes for that.

## 3. Include a `PluginInfo.json`

```json
{
  "FullName":      "My Plugin",
  "Description":   "What it does, in one line",
  "Version":       "1.0.0",
  "MinApiVersion": 2,
  "Dependencies":  ["Permission"]
}
```

This isn't a formality. `MinApiVersion` makes the loader **refuse** your plugin
on an API that's too old, instead of letting it run against a struct that
changed size. And when somebody asks for help on a forum, the first question
("which version are you on?") is already answered in the server log.

---

## Mistakes we see a lot

**Storing a file at a relative path.** `fopen("data.db", "w")` writes wherever
the **server** is, not where your plugin is. Use
`api->CaminhoDados("YourPlugin", "data.db")`.

**Asking Permission during load.** At that moment it may not have come up yet,
and you'll conclude nobody installed it. Ask when the player uses the feature.

**Doing work in `DllMain`.** Windows holds a global lock there and almost any
call will hang the whole process. Do everything in `ConanPluginCarregar`.

**Passing a `float` where the game wants a `double`.** The API refuses and tells
you — but it's worth knowing beforehand: **293 functions** in this build
corrupt the stack that way.

**Calling the API from another thread carelessly.** If your plugin has its own
thread for I/O, use `api->AgendarNaThreadDoJogo` to touch game objects. Touching
them straight from another thread crashes the server.

---

## Found a bug in the API?

Open an issue with: what you did, what you expected, what happened, and the
relevant chunk of `Conan-Api/Logs/ConanApi.log`.

If it crashed the server, the last few seconds of the log are worth more than
any description.
