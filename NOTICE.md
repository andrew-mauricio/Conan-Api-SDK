# What is under which licence

This repository — the **SDK** — is **MIT**. That covers everything here: the
headers, `ConanSDK.h`, the examples and Permission.

**In practice, for you:** compile it, change it, publish it, **sell it**. No
permission to ask for, nothing to pay, nothing to share back. The plugin is
yours and its licence is your choice.

## And the runtime?

**[Conan-Api](https://github.com/andrew-mauricio/Conan-Api)** — the loader, the
runtime and the binaries that run on the server — is under **its own licence**,
which is restrictive on one point: the API may not be resold, re-hosted, or
bundled into a commercial package.

That does not affect you while you write plugins. Not one line of the runtime
ends up in your binary: you talk to a table of function pointers at runtime,
which is exactly why these headers can be MIT without any of it spreading to
your code.

## Why the split exists

What you build is yours. The foundation stays with whoever maintains it, so that
there is **one** API with an upgrade path when the game changes — instead of five
diverging copies nobody can keep track of.

The link to this repository is free: share and index it wherever you like.

---

**Conan-Api is an independent, community-developed project. It is not affiliated
with, endorsed by, sponsored by, or supported by Funcom or Inflexion Games.**
*Conan Exiles* and all related marks are the property of Funcom.
