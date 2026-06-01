<h1> sd2snes ❤️ capas</h1>

<img src="gfx/showcase.gif" width="512" alt="Demonstração">

Cartucho multifuncional baseado em cartão SD para o SNES

**🌐 Idioma:** [English 🇺🇸](README.md) · Português

> **Aviso**
>
> Este fork fornece um **menu multi-idioma (Português do Brasil + Inglês, alternável no menu)** e a **funcionalidade de capas dos jogos** para o firmware do sd2snes, baseado no [repositório original](https://github.com/mrehkopf/sd2snes) de [@mrehkopf](https://github.com/mrehkopf).
>
> Para dúvidas e bugs sobre a **tradução / seletor de idioma** ou as **capas**, use este repositório. Para qualquer outra coisa relacionada ao firmware em si, por favor utilize o repositório original.

## ✨ Destaques

Este fork é baseado no projeto original do sd2snes e também incorpora contribuições da comunidade do repositório original.

- 🌐 **Multi-idioma (Português + Inglês)** — menu, mensagens e telas do firmware totalmente traduzidos para o Português do Brasil, com um **seletor de idioma** no menu para alternar entre Português e o Inglês original na hora. Veja a seção **Idiomas** abaixo.
- 🎮 **Capas dos jogos** — exibe a capa (box-art) do jogo no menu enquanto você navega pela lista. Veja a seção **Capas dos Jogos** abaixo.
- 🎵 **Música de fundo no menu** — toca uma faixa `.spc` enquanto você navega pelo menu. Veja a seção **Música do Menu** abaixo.
- 🩹 **Patches IPS/BPS** — aplica patches de tradução/hack a um jogo no boot, sem modificar a ROM. Veja a seção **Patches IPS/BPS** abaixo. Por [@Xeroxxx](https://github.com/mrehkopf/sd2snes/pull/293) e modificado por mim.
- 🔄 **Reset para o menu — de volta à sua pasta/ROM** — um reset curto pode te levar direto à pasta, ou até à ROM exata, que você estava jogando. Veja a seção **Reset para o Menu** abaixo. Por [@Xeroxxx](https://github.com/mrehkopf/sd2snes/pull/293).

## Instalação

A tradução está disponível na seção [**Releases**](https://github.com/ludufre/sd2snes/releases) deste repositório. O pacote contém **apenas os arquivos traduzidos**, portanto é necessário instalar antes o firmware oficial completo, disponível em [sd2snes.de/blog/downloads](https://sd2snes.de/blog/downloads).

O nome de cada Release indica a versão equivalente do firmware oficial. Por exemplo: a release **"v2.1 (sd2snes v1.11.2)"** corresponde ao firmware oficial `v1.11.2`.

1. Baixe e instale o firmware oficial da versão correspondente em [sd2snes.de/blog/downloads](https://sd2snes.de/blog/downloads).
2. Baixe o `.zip` da tradução em [Releases](https://github.com/ludufre/sd2snes/releases), escolhendo a versão equivalente ao firmware instalado.
3. Extraia o conteúdo do `.zip` dentro da pasta `/sd2snes` do seu cartão SD, substituindo os arquivos existentes.
4. Insira o cartão no seu sd2snes/FXPAK e ligue o console. O menu carrega em Português do Brasil por padrão — troque para Inglês (ou de volta) a qualquer momento pela opção **Idioma** no menu principal.

## 🌐 Idiomas

Este fork não é só uma tradução em Português do Brasil — ele traz um **seletor de idioma** para o menu rodar em **Português** ou no **Inglês original**, alternável a qualquer momento.

Alterne pela opção **Idioma** no menu principal — a troca é aplicada na hora e sua escolha é salva para o próximo boot.

- **Português** — a tradução completa em Português do Brasil (menu, mensagens, telas).
- **English** — o idioma original do firmware.

---

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

> [!IMPORTANT]
> Gere as capas com o **[sd2snes-covers](https://github.com/ludufre/sd2snes-covers) v1.1.0 ou superior**. Este firmware (**v1.11.2-br-2.1 ou superior**) usa o novo formato `.cov` de sprites OBJ — capas criadas com versões anteriores do app não serão exibidas corretamente; basta regerá-las com a v1.1.0+.

---

## 🩹 Patches IPS/BPS

Este fork pode aplicar patches **IPS** e **BPS** (traduções, hacks, correções) a um jogo **na hora de carregar**. O patch é aplicado apenas à cópia da ROM na memória — **o arquivo original da ROM no cartão SD nunca é modificado**.

Coloque o arquivo de patch na **mesma pasta** da ROM, com um nome que **comece com o nome da ROM** (sem a extensão) e termine em **`.ips`** ou **`.bps`**:

```
/sd2snes/A/Aladdin (USA).sfc
/sd2snes/A/Aladdin (USA).ips          ← um patch para este jogo
/sd2snes/A/Aladdin (USA) (Hack).bps   ← outro patch para o mesmo jogo
```

Ao abrir um jogo que tenha patches correspondentes, o menu mostra um **seletor de patch** antes de iniciar:

- **`[Sem patch]`** é sempre a primeira opção — inicia o jogo sem patch.
- Escolha um patch para aplicá-lo neste boot.
- Até **8** patches por jogo são listados (ordenados por nome).

`.ips` e `.bps` são detectados automaticamente. Patches BPS podem gerar uma ROM maior (isso é tratado automaticamente).

**Verificar integridade:** há uma opção no menu em **Configurações → Opções de Patches → Verificar Integridade**. Quando ligada (padrão), o firmware relê a ROM já aplicada para confirmar que o patch foi aplicado corretamente. Isso deixa o carregamento bem mais lento (~23 s para um BPS de 4 MB), então desligue se preferir carregamentos mais rápidos.

---

## 🎵 Música do Menu

O menu pode tocar uma música de fundo enquanto você navega pela lista de jogos. A música é qualquer arquivo **`.spc`** (o formato de música nativo do SNES) colocado neste caminho exato no cartão SD:

```
/sd2snes/menu.spc
```

**Como definir ou trocar a música:**

1. Baixe um arquivo `.spc` (veja onde abaixo).
2. Renomeie para `menu.spc`.
3. Copie para a pasta `/sd2snes/` do seu cartão SD.
4. Pronto — a música toca no próximo boot do menu.

**Onde baixar arquivos `.spc`:**

- [snesmusic.org](https://snesmusic.org) — trilhas sonoras de jogos de SNES.
- [zophar.net/music](https://www.zophar.net/music/nintendo-snes-spc) — coleções de SPC por jogo.

> [!TIP]
> Muitos sites distribuem as trilhas em `.rsn` (um arquivo `.rar` contendo vários `.spc` dentro). Nesse caso, extraia o `.rsn` e escolha um dos `.spc` de dentro.

**Ligar/desligar:** há um interruptor no menu em **Configurações → Opções do Navegador → Música do menu**. Quando ligada (padrão), o menu procura por `/sd2snes/menu.spc`; se o arquivo não existir, o menu simplesmente fica em silêncio (sem erro).

**Observações:**

- Só funciona o formato `.spc` (não MP3/WAV). Um `.spc` não é uma gravação de áudio — é um "snapshot" do chip de som do SNES (amostras + sequência), limitado a 64 KB. **Não existe conversão de MP3 para `.spc`**; use trilhas `.spc` prontas.
- Ao carregar a música (no boot, após um reset ou ao ligar o interruptor) há uma breve pausa de ~1 s enquanto os 64 KB são enviados ao chip de som. Isso é uma limitação do hardware (a transferência precisa rodar com as interrupções desligadas).
- Ao abrir um `.spc` pelo navegador de arquivos, a música de fundo é pausada automaticamente e retomada quando você volta (tecla B).

---

## 🔄 Reset para o Menu

O firmware pode te levar **de volta ao menu** quando você aperta o botão de reset do console (um reset curto), em vez de reiniciar o jogo. Este fork amplia essa opção para que você não caia sempre no topo da lista — ele pode te levar **direto para onde você estava**.

Configure no menu em **Configurações → Opções no Jogo → Reset para o menu**. Há quatro modos:

- **Des** — o botão de reset apenas reinicia o jogo (comportamento padrão do SNES).
- **Lig** — um reset curto volta ao menu.
- **Pasta** *(novo)* — volta ao menu **e abre a pasta** do jogo que você estava jogando.
- **ROM** *(novo)* — tudo que o **Pasta** faz, **e ainda** pré-seleciona (destaca) a ROM que você estava jogando, para você reabri-la — ou pular para uma vizinha — com um único toque.

> [!NOTE]
> Os modos **Pasta** e **ROM** só reposicionam a lista após um reset de volta ao menu — ligar o console do zero ainda começa no topo, como de costume. Sua escolha é salva na configuração e mantida entre reinícios.

---

## ⚖️ Licença & Código-fonte

Este projeto continua licenciado sob a GNU General Public License v2.0 (GPL-2.0), seguindo a licença do projeto original sd2snes.

Todos os direitos autorais originais pertencem aos seus respectivos autores e contribuidores.

Modificações adicionais neste fork:
Copyright (C) 2026 Luan Freitas

O código-fonte de todos os binários/releases distribuídos está disponível neste repositório e nas tags/releases correspondentes do Git, em conformidade com os requisitos da GPL.

Veja o [README original](https://github.com/mrehkopf/sd2snes/blob/master/README.md)

Veja o [README do FURiOUS](README.Savestates.FURiOUS.md) para informações sobre Save States!
