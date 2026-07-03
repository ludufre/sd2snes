<h1> sd2snes+</h1>

<img src="gfx/showcase.gif" width="512" alt="Démonstration">
<img src="gfx/langs.gif" width="384" alt="Le menu principal en Anglais, Portugais du Brésil, Espagnol, Allemand et Français">

Une expérience de firmware plus conviviale pour sd2snes/FXPAK : langues, jaquettes de jeux, musique de menu, patchs et un retour au menu plus pratique.

**🌐 Langue :** [English 🇺🇸](README.md) · [Português 🇧🇷](README-BR.md) · [Español 🇪🇸](README-ES.md) · [Deutsch 🇩🇪](README-DE.md) · Français

**Plus d'informations :** le site [sd2snes.ludufre.com](https://sd2snes.ludufre.com) réunit davantage de détails, de guides et d'exemples visuels sur ce fork.

> **Qu'est-ce que c'est ?**
>
> Ceci est un fork du [firmware original de sd2snes](https://github.com/mrehkopf/sd2snes) par [@mrehkopf](https://github.com/mrehkopf). Il conserve la base du firmware original et ajoute des améliorations orientées utilisateur : un menu en Portugais du Brésil, Anglais, Espagnol, Allemand et Français, des jaquettes de jeux, de la musique de menu, la sélection de patchs IPS/BPS et de meilleures options de retour au menu.
>
> Utilisez ce dépôt pour les questions ou bugs concernant la **traduction**, le **sélecteur de langue**, les **jaquettes**, la **musique du menu**, le **sélecteur de patchs** ou l'**éditeur de thèmes de ce fork**. Pour les problèmes du firmware principal sans rapport avec ces ajouts, merci d'utiliser le projet original.

## Pour commencer

Si vous voulez simplement utiliser ce firmware, vous n'avez **pas** besoin de compiler quoi que ce soit ni de télécharger le code source.

Il vous faut :

- Une cartouche sd2snes ou FXPAK.
- Une carte SD déjà préparée pour votre cartouche.
- Le `.zip` **full** (complet) depuis la page [Releases](https://github.com/ludufre/sd2snes/releases) de ce dépôt.

> [!NOTE]
> Ce projet ne contient pas de jeux/ROMs. Utilisez vos propres fichiers obtenus légalement.

> [!IMPORTANT]
> **Matériel Mk.II :** le sd2snes original (Mk.II) dispose d'une flash de programme MCU limitée. Depuis la **v2.12**, le bootstrap FPGA est chargé depuis la carte SD au lieu du firmware, ce qui a libéré environ **21 Ko** et offre au Mk.II nettement plus de marge. Il reste la plus étroite des deux cartes, donc certaines fonctionnalités futures pourraient finir **Mk.III / FXPAK PRO uniquement** (ou désactivées sur Mk.II) par manque de place. Le Mk.III / FXPAK PRO n'est pas concerné, et tout ce qui se trouve dans la release actuelle fonctionne sur les deux.

## Ce que ce fork ajoute

- **Langues :** choisissez le Portugais du Brésil, l'Anglais, l'Espagnol, l'Allemand ou le Français directement dans le menu.
- **Descriptions des options :** une courte ligne d'aide traduite pour l'option de menu sélectionnée, affichée dans une boîte flottante (placée automatiquement au-dessus ou en dessous du menu).
- **Jaquettes de jeux :** affiche la jaquette de chaque jeu pendant que vous parcourez votre liste de ROMs.
- **Écran d'infos du jeu :** avant qu'une ROM démarre, une page de détails montre sa jaquette, une capture d'écran et des métadonnées (développeur, année, joueurs, genre, puce spéciale) plus une courte description, lues depuis `/sd2snes/info/<rom>.yml`. Avec l'écran activé, il s'affiche pour chaque ROM : une ROM sans entrée d'infos obtient quand même un écran avec son nom de fichier comme titre et — s'il existe une jaquette `<rom>.cov` voisine — cette jaquette est montrée là où la jaquette `.gd` apparaîtrait. Pour une **capture d'écran animée**, déposez un `<rom>.fmv` (généré à partir de n'importe quelle vidéo avec `utils/gen_fmv.py`) à côté de la jaquette — la boîte de capture le joue en boucle tandis que la jaquette `<rom>.cov` reste à côté. À activer dans *Paramètres du navigateur* (activé par défaut), où deux sous-options (grisées quand c'est désactivé) déterminent si le clip animé est joué — sinon une capture statique s'affiche — et si sa bande-son est jouée.
- **Musique du menu :** joue une piste `.spc` en fond pendant la navigation.
- **Sons du menu :** effets sonores de navigation optionnels (curseur, confirmation, retour, erreur) joués sur le DAC audio de la cartouche, indépendamment de la musique.
- **Patchs IPS/BPS :** choisissez des patchs de traduction, de hack ou de correction avant qu'un jeu démarre, sans modifier le fichier ROM sur la carte SD.
- **Gestionnaire de triches :** le sd2snes original applique déjà les triches — ce fork ajoute un menu pour **activer et désactiver** les codes d'un jeu sur la console (depuis `/sd2snes/cheats/<rom>.yml`), sans éditer le YAML sur un PC. Des triches prêtes à l'emploi peuvent être exportées depuis [gamehacking.org](https://gamehacking.org/system/snes) au format « FXPak Pro 1.7 (.yml) », ou téléchargées automatiquement par l'application **sd2snes Covers** (associées par CRC32).
- **Supprimer fichier et sauvegarde :** supprimez le fichier sélectionné ou juste sa sauvegarde (`.srm`) directement depuis le menu, sans retirer la carte SD.
- **Améliorations du retour au menu :** revenez au même dossier ou même à la même ROM après un reset court.
- **Thèmes (firmware 2.9+) :** choisissez un thème de menu — logo, couleurs, arrière-plan et barre de sélection — **directement sur la console**, depuis n'importe quel dossier de la carte. Téléchargez des thèmes prêts à l'emploi depuis la [galerie](https://sd2snes.ludufre.com/gallery/) ou créez le vôtre dans l'[éditeur web](https://sd2snes.ludufre.com/theme/).

## Installation

Pour l'installation la plus simple, téléchargez la release marquée **full** (complète). Elle inclut déjà tous les fichiers de firmware nécessaires à ce fork, vous n'avez donc **pas** besoin de télécharger d'abord le firmware officiel équivalent.

Le nom de chaque release indique la version du firmware officiel correspondant. Par exemple, **« v2.1 (sd2snes v1.11.2) »** est destinée au firmware officiel `v1.11.2`.

1. Téléchargez le `.zip` **full** correspondant depuis la page [Releases](https://github.com/ludufre/sd2snes/releases) de ce dépôt.
2. Ouvrez le `.zip` et copiez son contenu à la **racine de votre carte SD**.
3. Remplacez les fichiers existants lorsque votre ordinateur le demande.
4. Remettez la carte SD dans le sd2snes/FXPAK et allumez la console.

Si vous téléchargez à la place un paquet non-full, il ne contient que les fichiers modifiés par ce fork. Dans ce cas, installez d'abord le firmware officiel correspondant depuis [sd2snes.de/blog/downloads](https://sd2snes.de/blog/downloads), puis copiez les fichiers de ce fork dans le dossier `/sd2snes` par-dessus les fichiers existants.

Le menu démarre en **Anglais par défaut**. Vous pouvez en changer à tout moment via l'option **Langue** du menu principal.

## Être notifié des nouvelles versions

Pour recevoir une notification GitHub à chaque nouvelle version du firmware :

1. Ouvrez ce dépôt sur GitHub.
2. Cliquez sur **Watch**.
3. Choisissez **Custom**.
4. Sélectionnez **Releases** et enregistrez.

<p>
<img src="gfx/follow1.png" width="256" alt="Menu Watch de GitHub avec l'option Custom">
<img src="gfx/follow2.png" width="256" alt="Notifications personnalisées GitHub avec Releases sélectionné">
</p>

## Langues

<img src="gfx/langs.gif" width="512" alt="Le menu principal en Anglais, Portugais du Brésil, Espagnol, Allemand et Français">

Le menu peut fonctionner en cinq langues :

- **Português :** traduction en Portugais du Brésil pour les menus, messages et écrans.
- **English :** la langue originale et par défaut du firmware.
- **Español :** traduction en Espagnol pour les menus, messages et écrans.
- **Deutsch :** traduction en Allemand pour les menus, messages et écrans.
- **Français :** traduction en Français pour les menus, messages et écrans.

Ouvrez **Langue** dans le menu principal, choisissez la langue voulue, et le menu change immédiatement. Votre choix est conservé pour la prochaine mise sous tension de la console.

## Jaquettes de jeux

Les jaquettes apparaissent dans le menu pendant que vous parcourez vos jeux.

Pour chaque ROM, placez un fichier de jaquette dans le même dossier que la ROM. Le fichier de jaquette doit porter le même nom que la ROM, mais avec l'extension `.cov` :

```text
/sd2snes/A/Aladdin (USA).sfc
/sd2snes/A/Aladdin (USA).cov
```

Le moyen le plus simple de créer des fichiers `.cov` est l'application de génération de jaquettes :

### 👉 [github.com/ludufre/sd2snes-covers](https://github.com/ludufre/sd2snes-covers)

Utilisez **sd2snes-covers v1.1.0 ou plus récent**. Si vous avez créé des jaquettes avec une version plus ancienne, régénérez-les avec la nouvelle application.

Dans le menu, **Mostrar capas** / **Afficher les jaquettes** propose trois réglages : **Grand** (la jaquette complète), **Petit** (demi-taille — pratique quand une jaquette recouvre la liste de fichiers) et **Off**. *Grand* est le réglage par défaut, les configurations existantes ne sont donc pas affectées.

## Patchs IPS/BPS

Ce fork peut appliquer des patchs **IPS** et **BPS** au chargement d'un jeu. C'est utile pour les traductions de fans, les hacks et les corrections.

Votre fichier ROM sur la carte SD n'est pas modifié. Le patch est appliqué uniquement pendant le chargement du jeu.

Placez le patch dans le même dossier que la ROM. Son nom de fichier doit commencer par le nom de la ROM, sans l'extension de la ROM, et se terminer par `.ips` ou `.bps` :

```text
/sd2snes/A/Aladdin (USA).sfc
/sd2snes/A/Aladdin (USA).ips
/sd2snes/A/Aladdin (USA) (Hack).bps
```

Quand vous ouvrez un jeu ayant des patchs correspondants, le menu affiche un sélecteur de patch :

- **`[Pas de patch]`** démarre le jeu normalement.
- Choisissez un patch pour l'utiliser pour ce démarrage.
- Jusqu'à **8** patchs sont affichés par jeu.

## Musique et sons du menu

Le menu peut jouer une **musique de fond** pendant la navigation, ainsi que quatre **effets sonores de navigation** optionnels (curseur, confirmation, retour, erreur). Ils ne jouent que dans le menu et n'affectent jamais vos jeux.

Le moyen le plus simple de configurer les deux est le **Créateur de sons** web : choisissez la musique, fabriquez les effets et téléchargez les fichiers prêts à copier sur la carte. Tout tourne dans votre navigateur — rien n'est envoyé en ligne.

### 👉 [sd2snes.ludufre.com/sounds](https://sd2snes.ludufre.com/sounds/)

### Musique de fond (`menu.spc`)

La musique est un fichier **`.spc`** nommé `menu.spc`, placé ici :

```text
/sd2snes/menu.spc
```

Pour ajouter la musique à la main :

1. Téléchargez un fichier `.spc`.
2. Renommez-le en `menu.spc`.
3. Copiez-le dans le dossier `/sd2snes/` de votre carte SD.
4. Allumez la console.

De bons endroits pour trouver des fichiers `.spc` :

- [snesmusic.org](https://snesmusic.org)
- [zophar.net/music](https://www.zophar.net/music/nintendo-snes-spc) — propose un aperçu MP3 pour chaque piste, ce qui permet d'écouter avant de télécharger.

Activez ou désactivez la musique dans **Configuration → Paramètres du navigateur → Musique du menu**.

Vous pouvez aussi choisir la musique **sans rien renommer** : surlignez n'importe quel **`.spc`** dans le navigateur de fichiers, appuyez sur **Y** pour le menu contextuel et choisissez **Définir comme musique de menu**. Le menu se recharge avec cette piste comme nouvelle musique de fond et la mémorise entre les redémarrages ; `/sd2snes/menu.spc` reste le repli par défaut. Pour y revenir, utilisez **Configuration → Paramètres du navigateur → Restaurer la musique**.

> [!TIP]
> Certaines bandes-son se téléchargent sous forme de fichiers `.rsn`. Un `.rsn` est généralement une archive contenant plusieurs fichiers `.spc`. Extrayez-la et choisissez un `.spc` à l'intérieur.

### Sons de navigation (effets)

Quatre effets courts et optionnels jouent lorsque vous vous déplacez dans le menu. Chacun est un fichier séparé dans `/sd2snes/` :

| Fichier | Joue quand |
| --- | --- |
| `sfx_cursor.pcm` | le curseur bouge |
| `sfx_confirm.pcm` | vous ouvrez ou confirmez (A) |
| `sfx_back.pcm` | vous revenez en arrière (B) |
| `sfx_error.pcm` | une action n'est pas autorisée |

Ce sont des fichiers **MSU‑1 PCM** (16 bits stéréo, 44,1 kHz). Ils jouent sur le DAC audio de la cartouche, ils n'interrompent donc jamais la musique `.spc`. Un jeu d'effets par défaut est fourni avec le firmware, le menu a donc des sons dès le départ — utilisez le Créateur de sons ci-dessus pour les personnaliser ou les remplacer. (Un fichier manquant signifie simplement que cet effet reste silencieux.)

Activez ou désactivez les effets dans **Configuration → Paramètres du navigateur → Sons du menu**.

## Triches

Le firmware sd2snes original **applique** déjà les triches par jeu. Ce que ce fork ajoute, c'est un **gestionnaire de triches dans le menu**, pour que vous puissiez activer et désactiver des codes individuels sur la console — sans éditer le YAML sur un PC.

Les triches sont lues depuis un fichier **YAML** (`.yml`) dans le dossier `/sd2snes/cheats/`, nommé d'après la ROM (son extension remplacée par `.yml`) :

```text
/sd2snes/A/Aladdin (USA).sfc        ← la ROM (dans n'importe quel dossier)
/sd2snes/cheats/Aladdin (USA).yml   ← ses triches
```

Pour les gérer, surlignez une ROM dans le navigateur de fichiers, appuyez sur **Y** pour le menu contextuel et choisissez **Codes de triche**. La liste montre chaque code du fichier :

- **A** active ou désactive le code surligné.
- **B** enregistre vos modifications et quitte.

Les codes activés sont appliqués au prochain démarrage de ce jeu.

Pour obtenir des fichiers de triches prêts à l'emploi :

1. Ouvrez [gamehacking.org/system/snes](https://gamehacking.org/system/snes) et trouvez votre jeu.
2. Exportez ses codes au format **FXPak Pro 1.7 (.yml)**.
3. Renommez le fichier pour correspondre à la ROM et déposez-le dans `/sd2snes/cheats/` sur la carte SD.

> [!TIP]
> L'application **[sd2snes Covers](https://github.com/ludufre/sd2snes-covers)** peut récupérer des triches prêtes à l'emploi automatiquement — elle associe chaque ROM par CRC32 et enregistre des fichiers `<rom>.yml` dans un dossier `cheats/`, prêts à copier dans `/sd2snes/cheats/`.

> [!NOTE]
> Si une ROM n'a pas de `.yml` dans `/sd2snes/cheats/` (ou si le fichier n'a aucun code), le menu affiche un message « aucune triche pour cette ROM ».

## Menu de triche en jeu

Au-delà du gestionnaire de triches du menu, ce fork peut faire apparaître un **menu de triche par-dessus le jeu en cours**, pour que vous puissiez activer et désactiver des codes sans reset.

Pendant que vous jouez, maintenez **L + R + Y + Gauche** pour mettre en pause et ouvrir l'overlay :

- **Haut/Bas** déplace le curseur.
- **A** active ou désactive le code surligné.
- **B** ferme l'overlay et applique vos modifications au jeu en cours.

Il est **activé par défaut**. Désactivez-le dans **Configuration → Paramètres en jeu → Hook en jeu → Menu de triche**.

> [!TIP]
> L'application **[sd2snes Covers](https://github.com/ludufre/sd2snes-covers)** télécharge aussi les triches de chaque jeu automatiquement.

> [!NOTE]
> Limitations actuelles :
> - Seules les **64** premières triches sont listées.
> - Les modifications faites dans l'overlay ne sont **pas enregistrées** dans le fichier `.yml` — elles ne s'appliquent qu'à la session de jeu en cours.
> - Comme les savestates, il **ne fonctionne pas sur les jeux à puce spéciale** (SA-1, SuperFX, etc.).
> - La mise en page est encore rudimentaire (travail en cours).

## Supprimer fichier et sauvegarde

Vous pouvez supprimer des fichiers et des sauvegardes directement depuis le menu, sans retirer la carte SD ni utiliser un ordinateur.

Surlignez un fichier dans le navigateur et appuyez sur **Y** pour le menu contextuel :

- **Supprimer :** retire le fichier sélectionné.
- **Supprimer la sauvegarde :** retire uniquement la sauvegarde `.srm` de cette ROM, en conservant la ROM elle-même.

> [!WARNING]
> La suppression est définitive — il n'y a pas de corbeille sur la carte SD. Vérifiez bien le fichier sélectionné avant de confirmer.

## Retour au menu

Le bouton reset peut vous ramener au menu sd2snes au lieu de simplement redémarrer le jeu. Ce fork ajoute deux options qui facilitent le retour à votre liste de jeux.

Réglez-le dans **Configuration → Paramètres en jeu → Reset vers le menu** :

- **Off :** le reset se comporte comme un reset SNES normal.
- **On :** un reset court ramène au menu.
- **Dossier :** ramène au menu et ouvre le dossier du jeu auquel vous jouiez.
- **ROM :** revient au dossier et surligne la ROM à laquelle vous jouiez.

Les options **Dossier** et **ROM** fonctionnent après un reset vers le menu. Une mise sous tension complète démarre toujours depuis le menu principal de premier niveau.

## Thèmes

À partir du firmware **2.9**, vous pouvez changer toute l'apparence du menu — **logo, couleurs, arrière-plan et barre de sélection** — directement sur la console, sans PC.

1. Placez des fichiers de thème `.thm` dans **n'importe quel dossier** de votre carte SD — n'importe quel nom fonctionne, il ne peut simplement pas s'agir du dossier caché `/sd2snes`.
2. Dans le menu, ouvrez ce dossier et appuyez sur **A** sur un thème. Le menu se recharge avec le thème.
3. Pour revenir à l'apparence par défaut, allez dans **Configuration → Paramètres du navigateur → Restaurer le thème** dans le menu.

Vous pouvez obtenir des thèmes de deux façons :

**Galerie — thèmes prêts à l'emploi, un clic pour télécharger :**

### 👉 [sd2snes.ludufre.com/gallery](https://sd2snes.ludufre.com/gallery/)

**Créateur de thèmes — créez le vôtre :** téléversez un logo (avec transparence), choisissez les couleurs et téléchargez un `.thm`.

### 👉 [sd2snes.ludufre.com/theme](https://sd2snes.ludufre.com/theme/)

> [!NOTE]
> Avancé : il existe aussi un [éditeur de `m3nu.bin`](https://sd2snes.ludufre.com/theme/) qui patche un binaire de menu complet (l'ancien flux de travail). L'éditeur de thèmes officiel de sd2snes ne prend pas en charge le format de ce fork.

## Problèmes courants

**Le menu n'a pas changé après l'installation.**

Vérifiez que vous avez utilisé la release **full** correspondante et copié ses fichiers à la racine de la carte SD. Si vous avez utilisé un paquet non-full, vérifiez que le firmware officiel correspondant a bien été installé d'abord et que les fichiers de ce fork ont été copiés dans `/sd2snes`.

**Les jaquettes n'apparaissent pas.**

Vérifiez que les jaquettes sont activées, que chaque fichier `.cov` porte le même nom que sa ROM, et que les jaquettes ont été générées avec **sd2snes-covers v1.1.0 ou plus récent**.

**La musique du menu est silencieuse.**

Vérifiez que le fichier s'appelle exactement `menu.spc`, qu'il est placé dans `/sd2snes/menu.spc`, et qu'il s'agit bien d'un fichier `.spc`. Les fichiers MP3 et WAV ne fonctionnent pas.

**Les sons de navigation sont silencieux.**

Vérifiez que **Sons du menu** est activé, et que `sfx_cursor.pcm`, `sfx_confirm.pcm`, `sfx_back.pcm` et `sfx_error.pcm` sont dans `/sd2snes/` et sont des fichiers **MSU‑1 PCM**. Ils sont fournis avec le firmware ; si vous les avez retirés, recopiez-les depuis le paquet de release ou recréez-les avec le [Créateur de sons](https://sd2snes.ludufre.com/sounds/).

**Un patch n'apparaît pas.**

Vérifiez que le patch est dans le même dossier que la ROM, commence par le nom de fichier de la ROM et se termine par `.ips` ou `.bps`.

**Mk.II (Spartan‑3) : l'écran reste noir et une LED clignote comme une sirène.**

À partir de la **v2.12**, sur le **Mk.II** (FPGA Spartan‑3) le bootstrap FPGA (`fpga_mini`) est chargé depuis la carte SD au lieu d'être intégré au firmware, ce qui libère de l'espace flash pour les fonctionnalités du fork. (Le Mk.III / FXPAK PRO n'est pas concerné — il affiche toujours les messages à l'écran.) De ce fait, quand le Mk.II ne parvient pas à afficher l'écran de démarrage, il signale la raison avec les LEDs au lieu du texte. Ce n'est **pas** un dysfonctionnement :

- **Pas de carte SD** → sirène vert ↔ rouge. Insérez une carte et faites un cycle d'alimentation.

  <img src="misc/led_no_sd.gif" width="100" alt="Pas de carte SD — vert/rouge">

- **`/sd2snes/fpga_mini.bit` manquant ou illisible** → sirène vert ↔ jaune. Restaurez-le depuis le paquet de release et faites un cycle d'alimentation.

  <img src="misc/led_fpga_mini.gif" width="100" alt="Pas de FPGA — vert/jaune">

`fpga_mini.bit` est fourni avec la release v2.12+ — gardez simplement les fichiers de la release dans `/sd2snes/`.

## Notes avancées

Cette section s'adresse surtout aux développeurs, mainteneurs et utilisateurs avancés. Vous n'en avez pas besoin pour une installation normale.

### Format des jaquettes

Ce firmware, à partir de la **v1.11.2-br-2.1**, utilise le format de jaquette `.cov` plus récent basé sur les sprites OBJ. Les jaquettes générées par d'anciennes versions de l'application ne s'afficheront pas correctement. Régénérez-les avec **sd2snes-covers v1.1.0 ou plus récent**.

### Vérification d'intégrité des patchs BPS

La vérification d'intégrité BPS peut être activée dans **Configuration → Options de patch → Vérifier l'intégrité**.

Cette option est **désactivée par défaut**. Une fois activée, le firmware relit la ROM après avoir appliqué un patch BPS pour confirmer qu'il a été appliqué correctement. Cela rend le chargement BPS plus lent ; par exemple, un patch BPS de 4 Mo peut ajouter environ 15 secondes de temps de chargement en moyenne. Les patchs IPS ne sont pas vérifiés par cette option.

### Limitations de la musique et des sons du menu

Pour la musique, seuls les fichiers `.spc` sont pris en charge. Un fichier `.spc` n'est pas un enregistrement audio ordinaire ; c'est un instantané de l'état de la puce sonore du SNES, limité à 64 Ko. Il n'existe pas de conversion directe MP3-vers-SPC — le Créateur de sons vous permet de choisir et de prévisualiser un `.spc`, il n'en génère pas à partir d'un autre audio.

Quand la musique se charge au démarrage, après un reset ou après avoir activé l'option, le menu peut s'interrompre brièvement pendant que le fichier est envoyé à la puce sonore du SNES. Ouvrir un `.spc` depuis le navigateur de fichiers met en pause la musique de fond et la reprend quand vous revenez avec le bouton B.

Les effets de navigation sont séparés : ce sont de courts clips **MSU‑1 PCM** joués sur le DAC audio de la cartouche (16 bits stéréo, 44,1 kHz), la musique continue donc de jouer sur la puce sonore du SNES pendant qu'un effet se déclenche. Gardez-les courts (bien en dessous d'une seconde) pour qu'ils restent réactifs.

### Format des thèmes

L'éditeur de thèmes officiel attend la disposition du logo pleine largeur du sd2snes original. Ce fork a modifié l'en-tête du menu pour faire de la place aux jaquettes de jeux :

- Zone du logo originale : **256×56**
- Zone du logo de ce fork : **128×56**
- Le côté droit de l'en-tête est réservé à la jaquette du jeu
- Le logo utilise sa propre palette de couleurs

De ce fait, l'éditeur officiel ne peut pas lire ni écrire correctement le `m3nu.bin` de ce fork.

### Compiler depuis les sources

Tous les binaires de release sont compilés localement avec Docker. La seule exigence sur votre machine est Docker lui-même :

```
./build-docker.sh
```

Cela compile tout en une seule fois : le firmware MCU (`firmware.img` pour Mk.II, `firmware.im3` pour Mk.III, `firmware.stm` pour FXPAK PRO STM32), le menu SNES (`menu.bin` / `m3nu.bin`) et le compagnon ESP (`esp32.bin` / `esp8266.bin`). Il empaquette ensuite deux zips dans `release/` :

- `sd2snes_firmware_v<VER>.zip` : uniquement les binaires modifiés du fork
- `sd2snes_firmware_v<VER>-full.zip` : la base officielle sd2snes plus les binaires du fork

`<VER>` provient de `RELEASE_VERSION` dans `src/VERSION`.

Pour le travail au quotidien, `./build-docker.sh` suffit. L'image de build et les cores FPGA sont mis en cache, chaque exécution recompile donc juste le MCU, le menu et le compagnon. Autres options :

- `--reuse` : saute la compilation et ré-empaquette seulement ce qui est déjà dans `bin/`
- `--rebuild-image` : force la reconstruction de l'image de build Docker

Le firmware intègre le core de bootstrap FPGA (cfgware). Le core Mk.III/STM32 (`fpga_mini.bi3`, Cyclone IV) est synthétisé avec Intel Quartus et intégré à l'image de build automatiquement. Le core Mk.II (`fpga_mini.bit`, Spartan-3) est réutilisé depuis l'arbre par défaut. Pour le re-synthétiser avec Xilinx ISE dans Docker :

```
./build-docker.sh --with-ise
```

Cette étape est lourde et nécessite l'installateur Xilinx ISE 14.7 et une licence WebPACK dans `XILINX_SRC` (par défaut `~/Downloads`). Comme le fork ne modifie pas le FPGA, vous en avez rarement besoin.

### Crédits

La prise en charge des patchs IPS/BPS et le travail original sur le retour au menu proviennent de [@Xeroxxx](https://github.com/mrehkopf/sd2snes/pull/293), avec des modifications apportées dans ce fork.

Le travail sur le menu de triche en jeu provient de [@Relikk](https://github.com/Relikk).

Contributeurs du dépôt sd2snes original listés par GitHub :

- [@mrehkopf](https://github.com/mrehkopf)
- [@RedGuyyyy](https://github.com/RedGuyyyy)
- [@Relikk](https://github.com/Relikk)
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

### Code source et licence

Ce projet reste sous licence GNU General Public License v2.0 (GPL-2.0), suivant la licence du projet sd2snes original.

Tous les droits d'auteur originaux appartiennent à leurs auteurs et contributeurs respectifs.

Modifications spécifiques au fork :
Copyright (C) 2026 Luan Freitas et contributeurs

Le code source de tous les binaires/releases distribués est disponible dans ce dépôt et les tags/releases Git correspondants, conformément aux exigences de la GPL.

Consultez le [README original](https://github.com/mrehkopf/sd2snes/blob/master/README.md).

Consultez le [README de FURiOUS](README.Savestates.FURiOUS.md) pour des informations sur les Save States.
