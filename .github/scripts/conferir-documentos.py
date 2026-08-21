# -*- coding: utf-8 -*-
"""Guard for this repository's documents.

WHY IT EXISTS: on 2026-08-18 commit 269903a (v1.1.0) deleted 11 files and added
zero -- README.md, both language mirrors under Docs/, LICENSE, .gitignore,
PUBLICAR-PLUGIN.md and the five images in .github/imagens/. None of them came
back in the six commits that followed. The repository sat there with no front
page, no licence, and a language switcher pointing at files that no longer
existed, and nobody noticed for twelve hours. Nothing was watching.

WHAT IT GUARANTEES: that the documents exist, that every local path they
mention points at a real file, and that the three languages keep the same
shape. It DETECTS and complains; it does NOT block the push.

Usage:  python .github/scripts/conferir-documentos.py [repo-root]

Exit codes:  0 = passed | 1 = FAILED | 2 = DID NOT CHECK (which is not a pass).
"""
import io
import os
import re
import sys

# The 11 that vanished, plus the documents added since.
#
# ENGLISH IS THE PRIMARY LANGUAGE HERE. Portuguese lives beside it as .pt.md /
# LEIA-ME.txt. This list was rewritten on 2026-08-20, when the flip happened:
# it still demanded Docs/README.en.md, which correctly stopped existing the
# moment English moved to the main README.md -- so the guard failed three
# pushes in a row and painted a red X on a public repository for reasons that
# had nothing to do with the documents being broken.
#
# A guard that cries wolf gets ignored, and then it protects nothing.
# REQUIRED: the English documents and the page's assets. English is the primary
# language and the one that is kept — if one of these goes, readers lose the
# document, not a convenience.
PROTEGIDOS = [
    "README.md",                          # English, the front page
    "Docs/DEVELOPERS.md",                 # the dev guide, English
    "Docs/EVENTS.md",                     # the event map, English
    "README.txt",                         # SDK cover, English
    "PUBLISHING-A-PLUGIN.md",
    "LICENSE",
    ".gitignore",
    ".github/imagens/conan-header.jpg",
    ".github/imagens/conan-3.jpg",
    ".github/imagens/bandeiras/br.png",
    ".github/imagens/bandeiras/us.png",
    ".github/imagens/bandeiras/es.png",
]

# OPTIONAL: the translations. Welcome, and worth keeping current, but their
# absence is a note and not a defect — see check 2 for the asymmetry and why it
# is not symmetric.
OPCIONAIS = [
    "Docs/README.pt.md",
    "Docs/README.es.md",
    "Docs/PARA-DESENVOLVEDORES.pt.md",
    "Docs/EVENTOS.pt.md",
    "LEIA-ME.txt",
    "PUBLICAR-PLUGIN.pt.md",
]

# Every English document must have its Portuguese counterpart, and vice versa.
# A translation that silently disappears leaves the other one linking to
# nothing, which is how README.en.md rotted unnoticed.
PARES = [
    ("README.md", "Docs/README.pt.md"),
    ("Docs/DEVELOPERS.md", "Docs/PARA-DESENVOLVEDORES.pt.md"),
    ("Docs/EVENTS.md", "Docs/EVENTOS.pt.md"),
    ("README.txt", "LEIA-ME.txt"),
    ("PUBLISHING-A-PLUGIN.md", "PUBLICAR-PLUGIN.pt.md"),
]

IDIOMAS = ["README.md", "Docs/README.pt.md", "Docs/README.es.md"]

falhas = []
avisos = []
checagens = [0]


def ok(msg):
    checagens[0] += 1
    print("  [ok]    %s" % msg)


def erro(msg):
    checagens[0] += 1
    falhas.append(msg)
    print("  [FAIL]  %s" % msg)


def aviso(msg):
    checagens[0] += 1
    avisos.append(msg)
    print("  [warn]  %s" % msg)


def abortar(msg):
    print("")
    print("== 2 DID NOT CHECK: %s ==" % msg)
    print("   This is not a pass: the guard measured nothing.")
    sys.exit(2)


def ler(raiz, rel):
    caminho = os.path.join(raiz, rel)
    return io.open(caminho, encoding="utf-8").read().replace("\r\n", "\n")


def main():
    raiz = sys.argv[1] if len(sys.argv) > 1 else "."
    if not os.path.isdir(raiz):
        abortar("root does not exist: %s" % raiz)
    if not os.path.isdir(os.path.join(raiz, ".git")) and not os.environ.get("CI"):
        abortar("%s does not look like the repository root" % os.path.abspath(raiz))

    print("== 1. The protected documents exist ==")
    sumidos = []
    for rel in PROTEGIDOS:
        if os.path.isfile(os.path.join(raiz, rel)):
            ok(rel)
        else:
            erro("%s WAS DELETED" % rel)
            sumidos.append(rel)

    for rel in OPCIONAIS:
        if os.path.isfile(os.path.join(raiz, rel)):
            ok("%s (translation)" % rel)
        else:
            aviso("%s is not here (an optional translation)" % rel)

    if sumidos:
        print("")
        print("  To bring them back, from the last commit that still had them:")
        print("      git log --oneline --diff-filter=D -- %s" % sumidos[0])
        print("      git checkout <previous-commit> -- %s" % " ".join(sumidos))

    print("== 2. English is primary; Portuguese is an optional translation ==")
    #
    # THE RULE THIS ENCODES, and it is not symmetric.
    #
    # English is the language that is kept: every public document has an English
    # version and that is the one that stays current. Portuguese may sit beside
    # it as a translation, and it is welcome, but it is OPTIONAL — a missing or
    # lagging translation is not a broken repository.
    #
    # So the two directions are NOT the same failure:
    #
    #   English present, translation missing  -> a note. Nothing is broken; the
    #                                            document everyone can read is
    #                                            there.
    #   Translation present, English missing  -> a HARD failure. An orphaned
    #                                            .pt file means the PRIMARY
    #                                            document was lost, and the only
    #                                            thing left is the one most
    #                                            readers cannot use. That is the
    #                                            exact state this project spent
    #                                            an audit climbing out of.
    #
    # This check used to fail both ways, and it was wrong for the first: it
    # would paint the repository red over a file that was never required.
    for en, pt in PARES:
        tem_en = os.path.isfile(os.path.join(raiz, en))
        tem_pt = os.path.isfile(os.path.join(raiz, pt))
        if tem_en and tem_pt:
            ok("%s <-> %s" % (en, pt))
        elif tem_en:
            aviso("%s has no %s translation (optional)" % (en, pt))
        elif tem_pt:
            erro("%s is an ORPHANED translation: the English original %s is gone"
                 % (pt, en))
        else:
            erro("%s is missing (the English original, which is required)" % en)

    print("== 3. Every local path cited in the documents points at a real file ==")
    for rel in IDIOMAS:
        if not os.path.isfile(os.path.join(raiz, rel)):
            erro("%s missing: cannot check the paths inside it" % rel)
            continue
        txt = ler(raiz, rel)
        pasta = os.path.dirname(os.path.join(raiz, rel))
        alvos = set(re.findall(r"!\[[^\]]*\]\(([^)]+)\)", txt))
        alvos |= set(re.findall(r'src="([^"]+)"', txt))
        alvos |= set(re.findall(r'href="([^"]+\.md)"', txt))
        alvos = {a for a in alvos if not a.startswith(("http", "#"))}
        if not alvos:
            erro("%s: no local path found (guard with no target)" % rel)
            continue
        for alvo in sorted(alvos):
            if os.path.isfile(os.path.normpath(os.path.join(pasta, alvo))):
                ok("%s -> %s" % (rel, alvo))
            else:
                erro("%s -> %s DOES NOT EXIST" % (rel, alvo))

    print("== 4. The three languages still have the same shape ==")
    presentes = [f for f in IDIOMAS if os.path.isfile(os.path.join(raiz, f))]
    if len(presentes) != 3:
        erro("only %d of 3 languages present: cannot compare" % len(presentes))
    else:
        txt = {f: ler(raiz, f) for f in IDIOMAS}
        medidas = {
            "## sections": lambda s: len(re.findall(r"^## ", s, re.M)),
            "code fences": lambda s: len(re.findall(r"^```", s, re.M)),
            "table rows": lambda s: len(re.findall(r"^\|", s, re.M)),
            "centred blocks": lambda s: len(re.findall(r'<p align="center">', s)),
        }
        for nome, medir in medidas.items():
            valores = {f: medir(txt[f]) for f in IDIOMAS}
            if len(set(valores.values())) == 1:
                ok("%s: %d in all three" % (nome, valores[IDIOMAS[0]]))
            else:
                # A WARNING, NOT A FAILURE — and the difference is deliberate.
                #
                # A translation lagging behind the English page is a maintenance
                # item: the repository still works, every link still resolves,
                # nothing was deleted. Failing the build for it would paint a
                # public repository red for a reason that isn't a defect, and a
                # guard that cries wolf gets ignored — which is how this same
                # file spent three pushes red demanding a README.en.md that had
                # correctly stopped existing.
                #
                # What stays HARD: the documents existing, the EN/PT pairs, the
                # local paths resolving, the language switcher. Those are the
                # ones that broke for twelve hours in August and that nobody
                # noticed.
                aviso("%s differ between languages: %s" % (nome, valores))

        print("== 5. The language switcher points at the other two ==")
        esperado = {
            "README.md": ['href="Docs/README.pt.md"', 'href="Docs/README.es.md"'],
            "Docs/README.pt.md": ['href="../README.md"', 'href="README.es.md"'],
            "Docs/README.es.md": ['href="../README.md"', 'href="README.pt.md"'],
        }
        for f in IDIOMAS:
            for alvo in esperado[f]:
                if alvo in txt[f]:
                    ok("%s has %s" % (f, alvo))
                else:
                    erro("%s lost the link %s" % (f, alvo))

    print("")
    print("-------------------------------------------")
    print("checks: %d | failures: %d | warnings: %d"
          % (checagens[0], len(falhas), len(avisos)))
    if falhas:
        print("== 1 FAILED ==")
        return 1
    print("== 0 PASSED ==")
    return 0


sys.exit(main())
