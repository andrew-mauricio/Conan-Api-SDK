# -*- coding: utf-8 -*-
"""Guarda dos documentos do repositorio.

POR QUE ELA EXISTE: em 18/08/2026 o commit 269903a (v1.1.0) apagou 11 arquivos
e adicionou 0 -- README.md, os dois espelhos em Docs/, LICENSE, .gitignore,
PUBLICAR-PLUGIN.md e as cinco imagens de .github/imagens/. Nenhum foi recriado
nos seis commits seguintes. O repositorio ficou sem pagina inicial, sem licenca
e com o seletor de idioma apontando para arquivos que nao existiam mais, e
ninguem viu por doze horas. Nao havia nada que reclamasse.

O QUE ELA GARANTE: que os documentos existem, que todo caminho local citado por
eles aponta para arquivo de verdade, e que os tres idiomas continuam com a mesma
forma. Ela DETECTA e reclama; ela NAO impede o push.

Uso:  python .github/scripts/conferir-documentos.py [raiz-do-repo]

Estados:  0 = passou | 1 = REPROVOU | 2 = NAO VERIFICOU (que nao e aprovacao).
"""
import io
import os
import re
import sys

# Os 11 que sumiram, mais o proprio par de guarda.
PROTEGIDOS = [
    "README.md",
    "Docs/README.en.md",
    "Docs/README.es.md",
    "Docs/EVENTOS.md",
    "Docs/PARA-DESENVOLVEDORES.md",
    "LEIA-ME.txt",
    "LICENSE",
    ".gitignore",
    "PUBLICAR-PLUGIN.md",
    ".github/imagens/conan-header.jpg",
    ".github/imagens/conan-3.jpg",
    ".github/imagens/bandeiras/br.png",
    ".github/imagens/bandeiras/us.png",
    ".github/imagens/bandeiras/es.png",
]

IDIOMAS = ["README.md", "Docs/README.en.md", "Docs/README.es.md"]

falhas = []
checagens = [0]


def ok(msg):
    checagens[0] += 1
    print("  [ok]    %s" % msg)


def erro(msg):
    checagens[0] += 1
    falhas.append(msg)
    print("  [FALHA] %s" % msg)


def abortar(msg):
    print("")
    print("== 2 NAO VERIFICOU: %s ==" % msg)
    print("   Isso nao e aprovacao: a guarda nao mediu nada.")
    sys.exit(2)


def ler(raiz, rel):
    caminho = os.path.join(raiz, rel)
    return io.open(caminho, encoding="utf-8").read().replace("\r\n", "\n")


def main():
    raiz = sys.argv[1] if len(sys.argv) > 1 else "."
    if not os.path.isdir(raiz):
        abortar("raiz nao existe: %s" % raiz)
    if not os.path.isdir(os.path.join(raiz, ".git")) and not os.environ.get("CI"):
        abortar("%s nao parece a raiz do repositorio" % os.path.abspath(raiz))

    print("== 1. Os documentos protegidos existem ==")
    sumidos = []
    for rel in PROTEGIDOS:
        if os.path.isfile(os.path.join(raiz, rel)):
            ok(rel)
        else:
            erro("%s FOI APAGADO" % rel)
            sumidos.append(rel)

    if sumidos:
        print("")
        print("  Para trazer de volta, do ultimo commit que ainda os tinha:")
        print("      git log --oneline --diff-filter=D -- %s" % sumidos[0])
        print("      git checkout <commit-anterior> -- %s" % " ".join(sumidos))

    print("== 2. Todo caminho local citado nos documentos aponta para arquivo real ==")
    for rel in IDIOMAS:
        if not os.path.isfile(os.path.join(raiz, rel)):
            erro("%s ausente: nao da para conferir os caminhos dele" % rel)
            continue
        txt = ler(raiz, rel)
        pasta = os.path.dirname(os.path.join(raiz, rel))
        alvos = set(re.findall(r"!\[[^\]]*\]\(([^)]+)\)", txt))
        alvos |= set(re.findall(r'src="([^"]+)"', txt))
        alvos |= set(re.findall(r'href="([^"]+\.md)"', txt))
        alvos = {a for a in alvos if not a.startswith(("http", "#"))}
        if not alvos:
            erro("%s: nenhum caminho local encontrado (guarda sem alvo)" % rel)
            continue
        for alvo in sorted(alvos):
            if os.path.isfile(os.path.normpath(os.path.join(pasta, alvo))):
                ok("%s -> %s" % (rel, alvo))
            else:
                erro("%s -> %s NAO EXISTE" % (rel, alvo))

    print("== 3. Os tres idiomas continuam com a mesma forma ==")
    presentes = [f for f in IDIOMAS if os.path.isfile(os.path.join(raiz, f))]
    if len(presentes) != 3:
        erro("so %d dos 3 idiomas presentes: nao da para comparar" % len(presentes))
    else:
        txt = {f: ler(raiz, f) for f in IDIOMAS}
        medidas = {
            "secoes ##": lambda s: len(re.findall(r"^## ", s, re.M)),
            "cercas de codigo": lambda s: len(re.findall(r"^```", s, re.M)),
            "linhas de tabela": lambda s: len(re.findall(r"^\|", s, re.M)),
            "blocos centralizados": lambda s: len(re.findall(r'<p align="center">', s)),
        }
        for nome, medir in medidas.items():
            valores = {f: medir(txt[f]) for f in IDIOMAS}
            if len(set(valores.values())) == 1:
                ok("%s: %d nos tres" % (nome, valores[IDIOMAS[0]]))
            else:
                erro("%s divergem entre os idiomas: %s" % (nome, valores))

        print("== 4. O seletor de idioma aponta para os outros dois ==")
        esperado = {
            "README.md": ['href="Docs/README.en.md"', 'href="Docs/README.es.md"'],
            "Docs/README.en.md": ['href="../README.md"', 'href="README.es.md"'],
            "Docs/README.es.md": ['href="../README.md"', 'href="README.en.md"'],
        }
        for f in IDIOMAS:
            for alvo in esperado[f]:
                if alvo in txt[f]:
                    ok("%s tem %s" % (f, alvo))
                else:
                    erro("%s perdeu o link %s" % (f, alvo))

    print("")
    print("-------------------------------------------")
    print("checagens: %d | falhas: %d" % (checagens[0], len(falhas)))
    if falhas:
        print("== 1 REPROVOU ==")
        return 1
    print("== 0 PASSOU ==")
    return 0


sys.exit(main())
