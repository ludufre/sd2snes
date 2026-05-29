<h1> sd2snes ❤️ capas</h1>

<img src="gfx/showcase.gif" width="512" alt="Demonstração">

Cartucho multifuncional baseado em cartão SD para o SNES

**🌐 Idioma:** [English 🇺🇸](README.md) · Português

> **Aviso**
>
> Este fork fornece a **tradução em Português do Brasil** e a **funcionalidade de capas dos jogos** para o firmware do sd2snes, baseado no [repositório original](https://github.com/mrehkopf/sd2snes) de [@mrehkopf](https://github.com/mrehkopf).
>
> Para dúvidas e bugs sobre a **tradução** ou as **capas**, use este repositório. Para qualquer outra coisa relacionada ao firmware em si, por favor utilize o repositório original.

## ✨ Destaques

- 🇧🇷 **Tradução em Português do Brasil** — menu, mensagens e telas do firmware totalmente traduzidos.
- 🎮 **Capas dos jogos** — exibe a capa (box-art) do jogo no menu enquanto você navega pela lista. Veja a seção **Capas dos Jogos** abaixo.

## Instalação

A tradução está disponível na seção [**Releases**](https://github.com/ludufre/sd2snes/releases) deste repositório. O pacote contém **apenas os arquivos traduzidos**, portanto é necessário instalar antes o firmware oficial completo, disponível em [sd2snes.de/blog/downloads](https://sd2snes.de/blog/downloads).

O nome de cada Release indica a versão equivalente do firmware oficial. Por exemplo: a release **"Tradução v1.0 da Firmware v1.11.2"** corresponde ao firmware oficial `v1.11.2`.

1. Baixe e instale o firmware oficial da versão correspondente em [sd2snes.de/blog/downloads](https://sd2snes.de/blog/downloads).
2. Baixe o `.zip` da tradução em [Releases](https://github.com/ludufre/sd2snes/releases), escolhendo a versão equivalente ao firmware instalado.
3. Extraia o conteúdo do `.zip` dentro da pasta `/sd2snes` do seu cartão SD, substituindo os arquivos existentes.
4. Insira o cartão no seu sd2snes/FXPAK e ligue o console. O menu será carregado em Português do Brasil.

## 🎮 Capas dos Jogos

Além da tradução, este fork exibe a **capa (box-art) do jogo** no menu, enquanto você navega pela lista de jogos.

Para cada ROM, basta colocar um arquivo de capa **com o mesmo nome** e a extensão **`.cov`** na mesma pasta da ROM:

```
/sd2snes/A/Aladdin (USA).sfc
/sd2snes/A/Aladdin (USA).cov   ← a capa deste jogo
```

As capas usam um formato próprio (`.cov`). A maneira mais fácil de criá-las é com o **aplicativo multiplataforma** (Windows, macOS e Linux), disponível no repositório:

### 👉 [github.com/ludufre/sd2snes-covers](https://github.com/ludufre/sd2snes-covers)

O app gera os arquivos `.cov` para a sua biblioteca de jogos. No menu, a exibição das capas pode ser ligada/desligada pela opção **"Mostrar capas"**.

---

Veja o [README original](https://github.com/mrehkopf/sd2snes/blob/master/README.md)

Veja o [README do FURiOUS](README.Savestates.FURiOUS.md) para informações sobre Save States!
