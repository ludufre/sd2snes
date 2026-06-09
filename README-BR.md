<h1> sd2snes+</h1>

<img src="gfx/showcase.gif" width="512" alt="Demonstração">
<img src="gfx/langs.gif" width="384" alt="O menu principal em Inglês, Português do Brasil e Espanhol">

Uma experiência mais amigável de firmware para sd2snes/FXPAK: idiomas, capas de jogos, música no menu, patches e reset para o menu mais prático.

**🌐 Idioma:** [English 🇺🇸](README.md) · Português · [Español 🇪🇸](README-ES.md)

**Mais informações:** o hotsite [sd2snes.ludufre.com](https://sd2snes.ludufre.com) reúne mais detalhes, guias e exemplos visuais sobre este fork.

> **O que é isto?**
>
> Este é um fork do [firmware original do sd2snes](https://github.com/mrehkopf/sd2snes) de [@mrehkopf](https://github.com/mrehkopf). Ele mantém a base do firmware original e adiciona melhorias voltadas para quem usa o cartucho: menu em Português do Brasil, Inglês e Espanhol, capas de jogos, música no menu, seleção de patches IPS/BPS e opções melhores de reset para o menu.
>
> Use este repositório para dúvidas ou bugs sobre a **tradução**, o **seletor de idioma**, as **capas**, a **música do menu**, o **seletor de patches** ou o **editor de temas deste fork**. Para problemas do firmware principal que não tenham relação com essas adições, use o projeto original.

## Comece aqui

Se você só quer usar este firmware, **não** precisa compilar nada nem baixar o código-fonte.

Você precisa de:

- Um cartucho sd2snes ou FXPAK.
- Um cartão SD já preparado para o seu cartucho.
- O `.zip` **full** da página [Releases](https://github.com/ludufre/sd2snes/releases) deste repositório.

> [!NOTE]
> Este projeto não inclui jogos/ROMs. Use seus próprios arquivos obtidos legalmente.

## O que este fork adiciona

- **Idiomas:** escolha Português do Brasil, Inglês ou Espanhol diretamente no menu.
- **Capas dos jogos:** veja a capa de cada jogo enquanto navega pela lista de ROMs.
- **Música no menu:** toque uma faixa `.spc` de fundo enquanto navega.
- **Patches IPS/BPS:** escolha patches de tradução, hacks ou correções antes de iniciar um jogo, sem alterar a ROM no cartão SD.
- **Gerenciador de cheats:** o sd2snes original já aplica trapaças — este fork adiciona um menu pra **ativar e desativar** os códigos de um jogo no console (a partir de `/sd2snes/cheats/<rom>.yml`), sem editar o YAML no PC. Dá para baixar cheats prontos no [gamehacking.org](https://gamehacking.org/system/snes) exportando como "FXPak Pro 1.7 (.yml)".
- **Deletar arquivo e save:** apague o arquivo selecionado ou só o save (`.srm`) direto pelo menu, sem tirar o cartão SD.
- **Melhorias no reset para o menu:** volte para a mesma pasta ou até para a mesma ROM depois de um reset curto.
- **Temas (firmware 2.9+):** escolha o tema do menu — logo, cores, fundo e barra de seleção — **direto no console**, a partir de qualquer pasta do cartão. Baixe temas prontos na [galeria](https://sd2snes.ludufre.com/gallery/) ou crie o seu no [editor web](https://sd2snes.ludufre.com/theme/).

## Instalação

Para a instalação mais simples, baixe a release marcada como **full**. Ela já inclui todos os arquivos de firmware necessários para este fork, então você **não** precisa baixar antes o firmware oficial equivalente.

O nome de cada release mostra a versão correspondente do firmware oficial. Por exemplo, **"v2.1 (sd2snes v1.11.2)"** foi feita para o firmware oficial `v1.11.2`.

1. Baixe o `.zip` **full** correspondente na página [Releases](https://github.com/ludufre/sd2snes/releases) deste repositório.
2. Abra o `.zip` e copie o conteúdo para a **raiz do cartão SD**.
3. Substitua os arquivos existentes quando o computador perguntar.
4. Coloque o cartão SD de volta no sd2snes/FXPAK e ligue o console.

Se você baixar um pacote que não seja full, ele contém apenas os arquivos alterados por este fork. Nesse caso, instale primeiro o firmware oficial correspondente em [sd2snes.de/blog/downloads](https://sd2snes.de/blog/downloads) e depois copie os arquivos deste fork para a pasta `/sd2snes`, substituindo os arquivos existentes.

O menu inicia em **Inglês por padrão**. Você pode trocar o idioma a qualquer momento pela opção **Idioma** no menu principal.

## Receber notificações de novas versões

Para receber uma notificação do GitHub sempre que uma nova versão do firmware for lançada:

1. Abra este repositório no GitHub.
2. Clique em **Watch**.
3. Escolha **Custom**.
4. Marque **Releases** e salve.

<p>
<img src="gfx/follow1.png" width="256" alt="Menu Watch do GitHub com a opção Custom">
<img src="gfx/follow2.png" width="256" alt="Notificações customizadas do GitHub com Releases marcado">
</p>

## Idiomas

<img src="gfx/langs.gif" width="512" alt="O menu principal em Inglês, Português do Brasil e Espanhol">

O menu pode rodar em três idiomas:

- **Português:** tradução em Português do Brasil para menus, mensagens e telas.
- **English:** o idioma original e padrão do firmware.
- **Español:** tradução em Espanhol para menus, mensagens e telas.

Abra **Idioma** no menu principal, escolha o idioma desejado e o menu muda na hora. Sua escolha fica salva para a próxima vez que ligar o console.

## Capas dos jogos

As capas aparecem no menu enquanto você navega pelos jogos.

Para cada ROM, coloque um arquivo de capa na mesma pasta da ROM. O arquivo de capa precisa ter o mesmo nome da ROM, mas com a extensão `.cov`:

```text
/sd2snes/A/Aladdin (USA).sfc
/sd2snes/A/Aladdin (USA).cov
```

A forma mais fácil de criar arquivos `.cov` é usando o aplicativo gerador de capas:

### 👉 [github.com/ludufre/sd2snes-covers](https://github.com/ludufre/sd2snes-covers)

Use o **sd2snes-covers v1.1.0 ou mais novo**. Se você criou capas com uma versão mais antiga, gere as capas novamente com o app mais novo.

No menu, **Mostrar capas** tem três opções: **Grande** (capa inteira), **Pequeno** (metade do tamanho — útil quando a capa cobre a lista de arquivos) e **Desligado**. O padrão é **Grande**, então quem já usava as capas não precisa mudar nada.

## Patches IPS/BPS

Este fork pode aplicar patches **IPS** e **BPS** quando um jogo carrega. Isso é útil para traduções de fãs, hacks e correções.

A ROM no cartão SD não é alterada. O patch é aplicado apenas enquanto o jogo está sendo carregado.

Coloque o patch na mesma pasta da ROM. O nome do arquivo precisa começar com o nome da ROM, sem a extensão da ROM, e terminar em `.ips` ou `.bps`:

```text
/sd2snes/A/Aladdin (USA).sfc
/sd2snes/A/Aladdin (USA).ips
/sd2snes/A/Aladdin (USA) (Hack).bps
```

Ao abrir um jogo com patches correspondentes, o menu mostra um seletor de patch:

- **`[Sem patch]`** inicia o jogo normalmente.
- Escolha um patch para usá-lo neste boot.
- Até **8** patches são exibidos por jogo.

## Música do menu

O menu pode tocar música de fundo enquanto você navega. O arquivo precisa ser um **`.spc`** chamado `menu.spc` e ficar neste caminho:

```text
/sd2snes/menu.spc
```

Para adicionar música:

1. Baixe um arquivo `.spc`.
2. Renomeie para `menu.spc`.
3. Copie para a pasta `/sd2snes/` do cartão SD.
4. Ligue o console.

Bons lugares para encontrar arquivos `.spc`:

- [snesmusic.org](https://snesmusic.org)
- [zophar.net/music](https://www.zophar.net/music/nintendo-snes-spc)

Você pode ligar ou desligar a música em **Configurações → Opções do Navegador → Música do menu**.

> [!TIP]
> Algumas trilhas vêm como arquivos `.rsn`. Um `.rsn` geralmente é um arquivo compactado com vários `.spc` dentro. Extraia o `.rsn` e escolha um dos arquivos `.spc`.

## Trapaças (cheats)

O firmware original do sd2snes já **aplica** trapaças por jogo. O que este fork adiciona é um **gerenciador de trapaças no menu**, pra você ativar e desativar cada código no próprio console — sem editar o YAML no PC.

As trapaças são lidas de um arquivo **YAML** (`.yml`) na pasta `/sd2snes/cheats/`, com o nome da ROM (a extensão dela trocada por `.yml`):

```text
/sd2snes/A/Aladdin (USA).sfc        ← a ROM (em qualquer pasta)
/sd2snes/cheats/Aladdin (USA).yml   ← as trapaças dela
```

Para gerenciar, selecione uma ROM no navegador de arquivos, aperte **Y** para abrir o menu de contexto e escolha **Trapaças**. A lista mostra cada código do arquivo:

- **A** ativa ou desativa o código destacado.
- **B** salva as alterações e sai.

Os códigos ativados são aplicados na próxima vez que você iniciar aquele jogo.

Para conseguir arquivos de trapaça prontos:

1. Abra [gamehacking.org/system/snes](https://gamehacking.org/system/snes) e encontre o seu jogo.
2. Exporte os códigos usando o formato **FXPak Pro 1.7 (.yml)**.
3. Renomeie o arquivo para bater com a ROM e coloque-o em `/sd2snes/cheats/` no cartão SD.

> [!TIP]
> O app **[sd2snes-covers](https://github.com/ludufre/sd2snes-covers)** (v1.5.0+) baixa trapaças prontas automaticamente — identifica cada ROM pelo CRC32 e salva os arquivos `<rom>.yml` numa pasta `cheats/`, prontos para copiar em `/sd2snes/cheats/`.

> [!NOTE]
> Se a ROM não tiver um `.yml` em `/sd2snes/cheats/` (ou o arquivo não tiver códigos), o menu mostra a mensagem "Nenhuma trapaça definida para esta ROM".

## Deletar arquivo e save

Você pode deletar arquivos e saves direto pelo menu, sem tirar o cartão SD nem usar o computador.

Selecione um arquivo no navegador e aperte **Y** para abrir o menu de contexto:

- **Deletar:** remove o arquivo selecionado.
- **Deletar save:** remove só o save `.srm` daquela ROM, mantendo a ROM.

> [!WARNING]
> A exclusão é permanente — não existe lixeira no cartão SD. Confira o arquivo selecionado antes de confirmar.

## Reset para o menu

O botão de reset pode levar você de volta ao menu do sd2snes em vez de apenas reiniciar o jogo. Este fork adiciona duas opções para facilitar o retorno à sua lista de jogos.

Configure em **Configurações → Opções no Jogo → Reset para o menu**:

- **Des:** o reset se comporta como um reset normal do SNES.
- **Lig:** um reset curto volta para o menu.
- **Pasta:** volta para o menu e abre a pasta do jogo que você estava jogando.
- **ROM:** volta para a pasta e destaca a ROM que você estava jogando.

As opções **Pasta** e **ROM** funcionam depois de um reset de volta para o menu. Ao ligar o console do zero, ele ainda começa no menu inicial normal.

## Temas

A partir do firmware **2.9**, você troca todo o visual do menu — **logo, cores, fundo e barra de seleção** — direto no console, sem PC.

1. Coloque arquivos de tema `.thm` em **qualquer pasta** do cartão — qualquer nome serve, só não pode ser a pasta oculta `/sd2snes`.
2. No menu, entre nessa pasta e aperte **A** num tema. O menu recarrega tematizado.
3. Para voltar ao padrão, use **Configurações → Opções do Navegador → Restaurar tema** no menu.

Você consegue temas de duas formas:

**Galeria — temas prontos, baixe num clique:**

### 👉 [sd2snes.ludufre.com/gallery](https://sd2snes.ludufre.com/gallery/)

**Editor de temas — crie o seu:** suba uma logo (com transparência), escolha as cores e baixe um `.thm`.

### 👉 [sd2snes.ludufre.com/theme](https://sd2snes.ludufre.com/theme/)

> [!NOTE]
> Avançado: também há um [editor de `m3nu.bin`](https://sd2snes.ludufre.com/theme/) que patcheia o menu inteiro (fluxo antigo). O editor de temas oficial do sd2snes não suporta o formato deste fork.

## Problemas comuns

**O menu não mudou depois da instalação.**

Confira se você usou a release **full** correspondente e copiou os arquivos para a raiz do cartão SD. Se usou um pacote que não seja full, confira se o firmware oficial correspondente foi instalado primeiro e se os arquivos deste fork foram copiados para `/sd2snes`.

**As capas não aparecem.**

Confira se as capas estão ativadas, se cada arquivo `.cov` tem o mesmo nome da ROM e se as capas foram geradas com o **sd2snes-covers v1.1.0 ou mais novo**.

**A música do menu não toca.**

Confira se o arquivo se chama exatamente `menu.spc`, se está em `/sd2snes/menu.spc` e se realmente é um arquivo `.spc`. MP3 e WAV não funcionam.

**Um patch não aparece.**

Confira se o patch está na mesma pasta da ROM, começa com o nome da ROM e termina em `.ips` ou `.bps`.

## Notas avançadas

Esta seção é principalmente para desenvolvedores, mantenedores e usuários avançados. Você não precisa dela para a instalação normal.

### Formato das capas

Este firmware, a partir da versão **v1.11.2-br-2.1**, usa o formato `.cov` mais novo com sprites OBJ. Capas geradas por versões antigas do aplicativo de capas não aparecem corretamente. Gere as capas novamente com o **sd2snes-covers v1.1.0 ou mais novo**.

### Verificação de integridade de patches BPS

A verificação de integridade para BPS pode ser ativada em **Configurações → Opções de Patches → Verificar Integridade**.

Essa opção vem **Desativada por padrão**. Quando ligada, o firmware relê a ROM depois de aplicar um patch BPS para confirmar que ele foi aplicado corretamente. Isso deixa o carregamento de BPS mais lento; por exemplo, um patch BPS de 4 MB pode adicionar cerca de 15 segundos ao carregamento, em média. Patches IPS não são verificados por essa opção.

### Limitações da música do menu

Apenas arquivos `.spc` são suportados. Um arquivo `.spc` não é uma gravação de áudio comum; ele é um snapshot do estado do chip de som do SNES e tem limite de 64 KB. Não existe conversão direta de MP3 para SPC.

Quando a música carrega no boot, depois de um reset ou depois de ligar a opção, o menu pode pausar brevemente enquanto o arquivo é enviado para o chip de som do SNES. Abrir um `.spc` pelo navegador de arquivos pausa a música de fundo e retoma quando você volta com o botão B.

### Formato do tema

O editor de temas oficial espera o layout original do sd2snes, com logo ocupando toda a largura do cabeçalho. Este fork mudou o cabeçalho do menu para abrir espaço para as capas dos jogos:

- Área do logo original: **256×56**
- Área do logo neste fork: **128×56**
- O lado direito do cabeçalho fica reservado para a capa do jogo
- O logo usa uma paleta de cores própria

Por isso, o editor oficial não consegue ler nem gravar corretamente o `m3nu.bin` deste fork.

### Compilar a partir do código-fonte

Todos os binários de release são compilados localmente com Docker. O único requisito na sua máquina é o próprio Docker:

```
./build-docker.sh
```

Isso compila tudo de uma vez: o firmware da MCU (`firmware.img` para Mk.II, `firmware.im3` para Mk.III, `firmware.stm` para FXPAK PRO STM32), o menu do SNES (`menu.bin` / `m3nu.bin`) e o companion ESP (`esp32.bin` / `esp8266.bin`). Em seguida monta dois zips em `release/`:

- `sd2snes_firmware_v<VER>.zip`: apenas os binários modificados do fork
- `sd2snes_firmware_v<VER>-full.zip`: a base oficial do sd2snes mais os binários do fork

`<VER>` vem de `RELEASE_VERSION` em `src/VERSION`.

No dia a dia, `./build-docker.sh` basta. A imagem de build e os cores da FPGA ficam em cache, então cada execução só recompila a MCU, o menu e o companion. Outras flags:

- `--reuse`: pula a compilação e só reempacota o que já está em `bin/`
- `--rebuild-image`: força a reconstrução da imagem de build do Docker

O firmware embute o core de bootstrap da FPGA (cfgware). O core do Mk.III/STM32 (`fpga_mini.bi3`, Cyclone IV) é sintetizado com o Intel Quartus e embutido na imagem de build automaticamente. O core do Mk.II (`fpga_mini.bit`, Spartan-3) é reaproveitado da árvore por padrão. Para re-sintetizá-lo com o Xilinx ISE dentro do Docker:

```
./build-docker.sh --with-ise
```

Esse passo é pesado e precisa do instalador do Xilinx ISE 14.7 e de uma licença WebPACK em `XILINX_SRC` (padrão `~/Downloads`). Como o fork não altera a FPGA, você raramente precisa dele.

### Créditos

O suporte a patches IPS/BPS e o trabalho original de reset para o menu vêm de [@Xeroxxx](https://github.com/mrehkopf/sd2snes/pull/293), com alterações feitas neste fork.

Contribuidores do repositório original do sd2snes listados pelo GitHub:

- [@mrehkopf](https://github.com/mrehkopf)
- [@RedGuyyyy](https://github.com/RedGuyyyy)
- [@github-user-name](https://github.com/github-user-name)
- [@furious](https://github.com/furious)
- [@redacted173](https://github.com/redacted173)
- [@francois-berder](https://github.com/francois-berder)
- [@Godzil](https://github.com/Godzil)
- [@mlarouche](https://github.com/mlarouche)
- [@devinacker](https://github.com/devinacker)
- [@Xeroxxx](https://github.com/Xeroxxx)
- [@tcprescott](https://github.com/tcprescott)
- [@freelancer42](https://github.com/freelancer42)
- [@LuigiBlood](https://github.com/LuigiBlood)
- [@DevLaTron](https://github.com/DevLaTron)
- [@gasparitiago](https://github.com/gasparitiago)

### Código-fonte e licença

Este projeto continua licenciado sob a GNU General Public License v2.0 (GPL-2.0), seguindo a licença do projeto original sd2snes.

Todos os direitos autorais originais pertencem aos seus respectivos autores e contribuidores.

Alterações específicas deste fork:
Copyright (C) 2026 Luan Freitas e contribuidores

O código-fonte de todos os binários/releases distribuídos está disponível neste repositório e nas tags/releases correspondentes do Git, em conformidade com os requisitos da GPL.

Veja o [README original](https://github.com/mrehkopf/sd2snes/blob/master/README.md).

Veja o [README do FURiOUS](README.Savestates.FURiOUS.md) para informações sobre Save States.
