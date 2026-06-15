<h1> sd2snes+</h1>

<img src="gfx/showcase.gif" width="512" alt="Demostración">
<img src="gfx/langs.gif" width="384" alt="El menú principal en Inglés, Portugués de Brasil, Español y Alemán">

Una experiencia de firmware más amigable para sd2snes/FXPAK: idiomas, carátulas de juegos, música en el menú, parches y un regreso al menú más práctico.

**🌐 Idioma:** [English 🇺🇸](README.md) · [Português 🇧🇷](README-BR.md) · Español · [Deutsch 🇩🇪](README-DE.md)

**Más información:** el sitio [sd2snes.ludufre.com](https://sd2snes.ludufre.com) reúne más detalles, guías y ejemplos visuales sobre este fork.

> **¿Qué es esto?**
>
> Este es un fork del [firmware original de sd2snes](https://github.com/mrehkopf/sd2snes) de [@mrehkopf](https://github.com/mrehkopf). Mantiene la base del firmware original y añade mejoras pensadas para quien usa el cartucho: menú en Portugués de Brasil, Inglés, Español y Alemán, carátulas de juegos, música en el menú, selección de parches IPS/BPS y mejores opciones de regreso al menú.
>
> Usa este repositorio para dudas o errores relacionados con la **traducción**, el **selector de idioma**, las **carátulas**, la **música del menú**, el **selector de parches** o el **editor de temas de este fork**. Para problemas del firmware principal que no estén relacionados con estas funciones, usa el proyecto original.

## Empieza aquí

Si solo quieres usar este firmware, **no** necesitas compilar nada ni descargar el código fuente.

Necesitas:

- Un cartucho sd2snes o FXPAK.
- Una tarjeta SD ya preparada para tu cartucho.
- El `.zip` **full** de la página [Releases](https://github.com/ludufre/sd2snes/releases) de este repositorio.

> [!NOTE]
> Este proyecto no incluye juegos/ROMs. Usa tus propios archivos obtenidos legalmente.

> [!IMPORTANT]
> **Hardware Mk.II:** el sd2snes original (Mk.II) tiene poca memoria de programa en el MCU, y este firmware ya está cerca de ese límite. Algunas funciones futuras podrían quedar **solo en Mk.III / FXPAK PRO** (o desactivadas en Mk.II) por falta de espacio. El Mk.III / FXPAK PRO no se ve afectado, y todo en esta versión funciona en ambos.

## Qué añade este fork

- **Idiomas:** elige Portugués de Brasil, Inglés, Español o Alemán directamente desde el menú.
- **Descripciones de opciones:** una breve línea de ayuda traducida para la opción de menú seleccionada, en un cuadro flotante (colocado encima o debajo del menú automáticamente).
- **Carátulas de juegos:** ve la carátula de cada juego mientras navegas por tu lista de ROMs.
- **Pantalla de info del juego:** antes de que una ROM arranque, ve una pantalla de detalles con su carátula, una captura y los metadatos (desarrollador, año, jugadores, género, chip especial) más una breve descripción, leídos de `/sd2snes/info/<rom>.yml`. Las ROMs sin entrada de info arrancan directo. Se activa en *Opciones del Navegador* (activado por defecto).
- **Música en el menú:** reproduce una pista `.spc` de fondo mientras navegas.
- **Sonidos del menú:** efectos de sonido de navegación opcionales (cursor, confirmar, volver, error) que suenan en el DAC de audio del cartucho, independientes de la música.
- **Parches IPS/BPS:** elige parches de traducción, hacks o correcciones antes de iniciar un juego, sin modificar la ROM en la tarjeta SD.
- **Gestor de cheats:** el sd2snes original ya aplica trucos — este fork añade un menú para **activar y desactivar** los códigos de un juego en la consola (desde `/sd2snes/cheats/<rom>.yml`), sin editar el YAML en el PC. Puedes descargar cheats listos en [gamehacking.org](https://gamehacking.org/system/snes) exportando como "FXPak Pro 1.7 (.yml)", o descargarlos automáticamente con la app **sd2snes Covers** (identificados por CRC32).
- **Borrar archivo y partida:** borra el archivo seleccionado o solo su partida (`.srm`) directamente desde el menú, sin sacar la tarjeta SD.
- **Mejoras en el regreso al menú:** vuelve a la misma carpeta o incluso a la misma ROM después de un reset corto.
- **Temas (firmware 2.9+):** elige el tema del menú — logo, colores, fondo y barra de selección — **en la propia consola**, desde cualquier carpeta de la tarjeta. Descarga temas listos en la [galería](https://sd2snes.ludufre.com/gallery/) o crea el tuyo en el [editor web](https://sd2snes.ludufre.com/theme/).

## Instalación

Para la instalación más simple, descarga la release marcada como **full**. Ya incluye todos los archivos de firmware necesarios para este fork, así que **no** necesitas descargar antes el firmware oficial equivalente.

El nombre de cada release muestra la versión correspondiente del firmware oficial. Por ejemplo, **"v2.1 (sd2snes v1.11.2)"** está hecha para el firmware oficial `v1.11.2`.

1. Descarga el `.zip` **full** correspondiente desde la página [Releases](https://github.com/ludufre/sd2snes/releases) de este repositorio.
2. Abre el `.zip` y copia su contenido en la **raíz de la tarjeta SD**.
3. Reemplaza los archivos existentes cuando tu computadora lo pregunte.
4. Vuelve a colocar la tarjeta SD en el sd2snes/FXPAK y enciende la consola.

Si descargas un paquete que no sea full, contiene solo los archivos modificados por este fork. En ese caso, instala primero el firmware oficial correspondiente desde [sd2snes.de/blog/downloads](https://sd2snes.de/blog/downloads) y después copia los archivos de este fork en la carpeta `/sd2snes`, reemplazando los archivos existentes.

El menú inicia en **Inglés por defecto**. Puedes cambiar el idioma en cualquier momento desde la opción **Idioma** del menú principal.

## Recibir notificaciones de nuevas versiones

Para recibir una notificación de GitHub cada vez que se publique una nueva versión del firmware:

1. Abre este repositorio en GitHub.
2. Haz clic en **Watch**.
3. Elige **Custom**.
4. Marca **Releases** y guarda.

<p>
<img src="gfx/follow1.png" width="256" alt="Menú Watch de GitHub con la opción Custom">
<img src="gfx/follow2.png" width="256" alt="Notificaciones personalizadas de GitHub con Releases marcado">
</p>

## Idiomas

<img src="gfx/langs.gif" width="512" alt="El menú principal en Inglés, Portugués de Brasil, Español y Alemán">

El menú puede funcionar en cuatro idiomas:

- **Português:** traducción al Portugués de Brasil para menús, mensajes y pantallas.
- **English:** idioma original y predeterminado del firmware.
- **Español:** traducción al Español para menús, mensajes y pantallas.
- **Deutsch:** traducción al Alemán para menús, mensajes y pantallas.

Abre **Idioma** en el menú principal, elige el idioma que quieras y el menú cambia de inmediato. Tu elección queda guardada para la próxima vez que enciendas la consola.

## Carátulas de juegos

Las carátulas aparecen en el menú mientras navegas por tus juegos.

Para cada ROM, coloca un archivo de carátula en la misma carpeta que la ROM. El archivo de carátula debe tener el mismo nombre que la ROM, pero con la extensión `.cov`:

```text
/sd2snes/A/Aladdin (USA).sfc
/sd2snes/A/Aladdin (USA).cov
```

La forma más fácil de crear archivos `.cov` es usando la aplicación generadora de carátulas:

### 👉 [github.com/ludufre/sd2snes-covers](https://github.com/ludufre/sd2snes-covers)

Usa **sd2snes-covers v1.1.0 o más reciente**. Si creaste carátulas con una versión anterior, vuelve a generarlas con la aplicación más nueva.

En el menú, **Mostrar carátulas** tiene tres opciones: **Grande** (la carátula completa), **Pequeño** (la mitad del tamaño — útil cuando la carátula tapa la lista de archivos) y **No** (desactivadas). El valor por defecto es **Grande**, así que las configuraciones existentes no cambian.

## Parches IPS/BPS

Este fork puede aplicar parches **IPS** y **BPS** cuando se carga un juego. Esto es útil para traducciones de fans, hacks y correcciones.

La ROM en la tarjeta SD no se modifica. El parche se aplica solo mientras el juego se está cargando.

Coloca el parche en la misma carpeta que la ROM. El nombre del archivo debe empezar con el nombre de la ROM, sin la extensión de la ROM, y terminar en `.ips` o `.bps`:

```text
/sd2snes/A/Aladdin (USA).sfc
/sd2snes/A/Aladdin (USA).ips
/sd2snes/A/Aladdin (USA) (Hack).bps
```

Al abrir un juego con parches compatibles, el menú muestra un selector de parche:

- **`[Sin parche]`** inicia el juego normalmente.
- Elige un parche para usarlo en este arranque.
- Se muestran hasta **8** parches por juego.

## Música y sonidos del menú

El menú puede reproducir **música de fondo** mientras navegas, además de cuatro **efectos de sonido de navegación** opcionales (cursor, confirmar, volver, error). Solo suenan en el menú y nunca afectan a tus juegos.

La forma más fácil de configurar ambos es el **Creador de Sonidos** en la web: elige la música, crea los efectos y descarga los archivos listos para copiar a la tarjeta. Todo se ejecuta en tu navegador — no se sube nada a ningún sitio.

### 👉 [sd2snes.ludufre.com/sounds](https://sd2snes.ludufre.com/sounds/)

### Música de fondo (`menu.spc`)

La música es un archivo **`.spc`** llamado `menu.spc`, en esta ruta:

```text
/sd2snes/menu.spc
```

Para añadir música a mano:

1. Descarga un archivo `.spc`.
2. Renómbralo como `menu.spc`.
3. Cópialo en la carpeta `/sd2snes/` de la tarjeta SD.
4. Enciende la consola.

Buenos lugares para encontrar archivos `.spc`:

- [snesmusic.org](https://snesmusic.org)
- [zophar.net/music](https://www.zophar.net/music/nintendo-snes-spc) — tiene una vista previa en MP3 de cada pista, así que puedes escuchar antes de descargar.

Puedes activar o desactivar la música en **Configuración → Opciones del Navegador → Música del menú**.

También puedes elegir la música **sin renombrar nada**: selecciona cualquier **`.spc`** en el navegador de archivos, pulsa **Y** para el menú contextual y elige **Definir como música del menú**. El menú se recarga con esa pista como nueva música de fondo y la recuerda entre reinicios; `/sd2snes/menu.spc` queda como reserva. Para volver a él, usa **Configuración → Opciones del Navegador → Restaurar música**.

> [!TIP]
> Algunas bandas sonoras vienen como archivos `.rsn`. Un `.rsn` suele ser un archivo comprimido con varios `.spc` dentro. Extrae el `.rsn` y elige uno de los archivos `.spc`.

### Sonidos de navegación (efectos)

Cuatro efectos cortos y opcionales suenan mientras te mueves por el menú. Cada uno es un archivo separado en `/sd2snes/`:

| Archivo | Suena cuando |
| --- | --- |
| `sfx_cursor.pcm` | el cursor se mueve |
| `sfx_confirm.pcm` | abres o confirmas (A) |
| `sfx_back.pcm` | vuelves atrás (B) |
| `sfx_error.pcm` | una acción no está permitida |

Son archivos **MSU‑1 PCM** (16 bits estéreo, 44,1 kHz). Suenan en el DAC de audio del cartucho, así que nunca interrumpen la música `.spc`. Un conjunto por defecto viene con el firmware, así que el menú ya tiene sonidos de fábrica — usa el Creador de Sonidos de arriba para personalizarlos o cambiarlos. (Si falta un archivo, ese efecto queda en silencio.)

Puedes activar o desactivar los efectos en **Configuración → Opciones del Navegador → Sonidos del menú**.

## Trucos (cheats)

El firmware original de sd2snes ya **aplica** trucos por juego. Lo que este fork añade es un **gestor de trucos en el menú**, para activar y desactivar cada código en la propia consola — sin editar el YAML en el PC.

Los trucos se leen de un archivo **YAML** (`.yml`) en la carpeta `/sd2snes/cheats/`, con el nombre de la ROM (su extensión reemplazada por `.yml`):

```text
/sd2snes/A/Aladdin (USA).sfc        ← la ROM (en cualquier carpeta)
/sd2snes/cheats/Aladdin (USA).yml   ← sus trucos
```

Para gestionarlos, resalta una ROM en el navegador de archivos, pulsa **Y** para abrir el menú contextual y elige **Trucos**. La lista muestra cada código del archivo:

- **A** activa o desactiva el código resaltado.
- **B** guarda los cambios y sale.

Los códigos activados se aplican la próxima vez que inicies ese juego.

Para conseguir archivos de trucos listos:

1. Abre [gamehacking.org/system/snes](https://gamehacking.org/system/snes) y busca tu juego.
2. Exporta sus códigos usando el formato **FXPak Pro 1.7 (.yml)**.
3. Renombra el archivo para que coincida con la ROM y colócalo en `/sd2snes/cheats/` en la tarjeta SD.

> [!TIP]
> La app **[sd2snes Covers](https://github.com/ludufre/sd2snes-covers)** descarga trucos listos automáticamente — identifica cada ROM por su CRC32 y guarda los archivos `<rom>.yml` en una carpeta `cheats/`, listos para copiar en `/sd2snes/cheats/`.

> [!NOTE]
> Si una ROM no tiene un `.yml` en `/sd2snes/cheats/` (o el archivo no tiene códigos), el menú muestra el mensaje "no hay trucos para esta ROM".

## Borrar archivo y partida guardada

Puedes borrar archivos y partidas directamente desde el menú, sin sacar la tarjeta SD ni usar una computadora.

Resalta un archivo en el navegador y pulsa **Y** para abrir el menú contextual:

- **Borrar:** elimina el archivo seleccionado.
- **Borrar partida:** elimina solo la partida `.srm` de esa ROM, conservando la ROM.

> [!WARNING]
> El borrado es permanente — no hay papelera en la tarjeta SD. Revisa el archivo seleccionado antes de confirmar.

## Reset al menú

El botón de reset puede llevarte de vuelta al menú de sd2snes en lugar de simplemente reiniciar el juego. Este fork añade dos opciones para facilitar el regreso a tu lista de juegos.

Configúralo en **Configuración → Opciones en Juego → Reset al menú**:

- **Des:** el reset se comporta como un reset normal del SNES.
- **Act:** un reset corto vuelve al menú.
- **Carpeta:** vuelve al menú y abre la carpeta del juego que estabas jugando.
- **ROM:** vuelve a la carpeta y resalta la ROM que estabas jugando.

Las opciones **Carpeta** y **ROM** funcionan después de un reset de vuelta al menú. Al encender la consola desde cero, sigue iniciando en el menú principal normal.

## Temas

Desde el firmware **2.9**, cambias todo el aspecto del menú — **logo, colores, fondo y barra de selección** — en la propia consola, sin PC.

1. Pon archivos de tema `.thm` en **cualquier carpeta** de la tarjeta — vale cualquier nombre, solo no puede ser la carpeta oculta `/sd2snes`.
2. En el menú, entra en esa carpeta y pulsa **A** en un tema. El menú recarga con el tema.
3. Para volver al aspecto original, usa **Configuración → Opciones del Navegador → Restaurar tema** en el menú.

Puedes conseguir temas de dos formas:

**Galería — temas listos, descarga en un clic:**

### 👉 [sd2snes.ludufre.com/gallery](https://sd2snes.ludufre.com/gallery/)

**Editor de temas — crea el tuyo:** sube un logo (con transparencia), elige los colores y descarga un `.thm`.

### 👉 [sd2snes.ludufre.com/theme](https://sd2snes.ludufre.com/theme/)

> [!NOTE]
> Avanzado: también hay un [editor de `m3nu.bin`](https://sd2snes.ludufre.com/theme/) que parchea el menú completo (el flujo antiguo). El editor oficial de temas de sd2snes no soporta el formato de este fork.

## Problemas comunes

**El menú no cambió después de la instalación.**

Comprueba que usaste la release **full** correspondiente y copiaste los archivos en la raíz de la tarjeta SD. Si usaste un paquete que no sea full, comprueba que el firmware oficial correspondiente se instaló primero y que los archivos de este fork se copiaron en `/sd2snes`.

**Las carátulas no aparecen.**

Comprueba que las carátulas estén activadas, que cada archivo `.cov` tenga el mismo nombre que su ROM y que las carátulas se hayan generado con **sd2snes-covers v1.1.0 o más reciente**.

**La música del menú no suena.**

Comprueba que el archivo se llame exactamente `menu.spc`, que esté en `/sd2snes/menu.spc` y que realmente sea un archivo `.spc`. MP3 y WAV no funcionan.

**Los sonidos de navegación no suenan.**

Comprueba que la opción **Sonidos del menú** esté activada, y que `sfx_cursor.pcm`, `sfx_confirm.pcm`, `sfx_back.pcm` y `sfx_error.pcm` estén en `/sd2snes/` y sean archivos **MSU‑1 PCM**. Vienen con el firmware; si los borraste, cópialos de nuevo del paquete del release o recréalos en el [Creador de Sonidos](https://sd2snes.ludufre.com/sounds/).

**Un parche no aparece.**

Comprueba que el parche esté en la misma carpeta que la ROM, que empiece con el nombre de la ROM y que termine en `.ips` o `.bps`.

## Notas avanzadas

Esta sección es principalmente para desarrolladores, mantenedores y usuarios avanzados. No la necesitas para una instalación normal.

### Formato de las carátulas

Este firmware, a partir de la versión **v1.11.2-br-2.1**, usa el formato `.cov` más nuevo con sprites OBJ. Las carátulas generadas por versiones antiguas de la aplicación de carátulas no se muestran correctamente. Vuelve a generarlas con **sd2snes-covers v1.1.0 o más reciente**.

### Verificación de integridad de parches BPS

La verificación de integridad para BPS se puede activar en **Configuración → Opciones de Patches → Verificar Integridad**.

Esta opción viene **Desactivada por defecto**. Cuando está activada, el firmware vuelve a leer la ROM después de aplicar un parche BPS para confirmar que se aplicó correctamente. Esto hace que la carga de BPS sea más lenta; por ejemplo, un parche BPS de 4 MB puede añadir unos 15 segundos a la carga, en promedio. Los parches IPS no son verificados por esta opción.

### Limitaciones de la música y los sonidos del menú

Para la música, solo se admiten archivos `.spc`. Un archivo `.spc` no es una grabación de audio común; es una instantánea del estado del chip de sonido del SNES y tiene un límite de 64 KB. No existe una conversión directa de MP3 a SPC — el Creador de Sonidos te deja elegir y escuchar un `.spc`, pero no genera uno a partir de otro audio.

Cuando la música se carga al iniciar, después de un reset o después de activar la opción, el menú puede pausarse brevemente mientras el archivo se envía al chip de sonido del SNES. Abrir un `.spc` desde el navegador de archivos pausa la música de fondo y la reanuda cuando vuelves con el botón B.

Los efectos de navegación son aparte: son clips cortos en **MSU‑1 PCM** reproducidos en el DAC de audio del cartucho (16 bits estéreo, 44,1 kHz), así que la música sigue sonando en el chip de sonido del SNES mientras se dispara un efecto. Manténlos cortos (bastante menos de un segundo) para que se sientan ágiles.

### Formato del tema

El editor oficial de temas espera el layout original de sd2snes, con el logo ocupando todo el ancho del encabezado. Este fork cambió el encabezado del menú para abrir espacio a las carátulas de los juegos:

- Área del logo original: **256×56**
- Área del logo en este fork: **128×56**
- El lado derecho del encabezado queda reservado para la carátula del juego
- El logo usa una paleta de colores propia

Por eso, el editor oficial no puede leer ni escribir correctamente el `m3nu.bin` de este fork.

### Compilar desde el código fuente

Todos los binarios de la release se compilan localmente con Docker. El único requisito en tu máquina es el propio Docker:

```
./build-docker.sh
```

Esto compila todo de una vez: el firmware del MCU (`firmware.img` para Mk.II, `firmware.im3` para Mk.III, `firmware.stm` para FXPAK PRO STM32), el menú de SNES (`menu.bin` / `m3nu.bin`) y el companion ESP (`esp32.bin` / `esp8266.bin`). Luego arma dos zips en `release/`:

- `sd2snes_firmware_v<VER>.zip`: solo los binarios modificados del fork
- `sd2snes_firmware_v<VER>-full.zip`: la base oficial de sd2snes más los binarios del fork

`<VER>` viene de `RELEASE_VERSION` en `src/VERSION`.

En el día a día, `./build-docker.sh` es todo lo que necesitas. La imagen de build y los cores de la FPGA quedan en caché, así que cada ejecución solo recompila el MCU, el menú y el companion. Otras flags:

- `--reuse`: omite la compilación y solo reempaqueta lo que ya está en `bin/`
- `--rebuild-image`: fuerza la reconstrucción de la imagen de build de Docker

El firmware incrusta el core de arranque de la FPGA (cfgware). El core del Mk.III/STM32 (`fpga_mini.bi3`, Cyclone IV) se sintetiza con Intel Quartus y se incrusta en la imagen de build automáticamente. El core del Mk.II (`fpga_mini.bit`, Spartan-3) se reutiliza del árbol por defecto. Para re-sintetizarlo con Xilinx ISE dentro de Docker:

```
./build-docker.sh --with-ise
```

Ese paso es pesado y necesita el instalador de Xilinx ISE 14.7 y una licencia WebPACK en `XILINX_SRC` (por defecto `~/Downloads`). Como el fork no cambia la FPGA, rara vez lo necesitas.

### Créditos

El soporte para parches IPS/BPS y el trabajo original de reset al menú vienen de [@Xeroxxx](https://github.com/mrehkopf/sd2snes/pull/293), con cambios realizados en este fork.

El trabajo del menú de trucos viene de [@Relikk](https://github.com/Relikk).

Contribuidores del repositorio original de sd2snes listados por GitHub:

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

### Código fuente y licencia

Este proyecto sigue licenciado bajo GNU General Public License v2.0 (GPL-2.0), siguiendo la licencia del proyecto original sd2snes.

Todos los derechos de autor originales pertenecen a sus respectivos autores y contribuidores.

Cambios específicos de este fork:
Copyright (C) 2026 Luan Freitas y contribuidores

El código fuente de todos los binarios/releases distribuidos está disponible en este repositorio y en las tags/releases correspondientes de Git, de acuerdo con los requisitos de la GPL.

Consulta el [README original](https://github.com/mrehkopf/sd2snes/blob/master/README.md).

Consulta el [README de FURiOUS](README.Savestates.FURiOUS.md) para información sobre Save States.
