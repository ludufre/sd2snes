<h1> sd2snes ❤️ carátulas</h1>

<img src="gfx/showcase.gif" width="512" alt="Demostración">
<img src="gfx/langs.gif" width="384" alt="El menú principal en Inglés, Portugués de Brasil y Español">

Una experiencia de firmware más amigable para sd2snes/FXPAK: idiomas, carátulas de juegos, música en el menú, parches y un regreso al menú más práctico.

**🌐 Idioma:** [English 🇺🇸](README.md) · [Português 🇧🇷](README-BR.md) · Español

**Más información:** el sitio [sd2snes.ludufre.com](https://sd2snes.ludufre.com) reúne más detalles, guías y ejemplos visuales sobre este fork.

> **¿Qué es esto?**
>
> Este es un fork del [firmware original de sd2snes](https://github.com/mrehkopf/sd2snes) de [@mrehkopf](https://github.com/mrehkopf). Mantiene la base del firmware original y añade mejoras pensadas para quien usa el cartucho: menú en Portugués de Brasil, Inglés y Español, carátulas de juegos, música en el menú, selección de parches IPS/BPS y mejores opciones de regreso al menú.
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

## Qué añade este fork

- **Idiomas:** elige Portugués de Brasil, Inglés o Español directamente desde el menú.
- **Carátulas de juegos:** ve la carátula de cada juego mientras navegas por tu lista de ROMs.
- **Música en el menú:** reproduce una pista `.spc` de fondo mientras navegas.
- **Parches IPS/BPS:** elige parches de traducción, hacks o correcciones antes de iniciar un juego, sin modificar la ROM en la tarjeta SD.
- **Mejoras en el regreso al menú:** vuelve a la misma carpeta o incluso a la misma ROM después de un reset corto.
- **Temas personalizados:** edita el logo, la fuente, la paleta, el fondo y los colores del selector con el editor de temas hecho para este fork.

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

<img src="gfx/langs.gif" width="512" alt="El menú principal en Inglés, Portugués de Brasil y Español">

El menú puede funcionar en tres idiomas:

- **Português:** traducción al Portugués de Brasil para menús, mensajes y pantallas.
- **English:** idioma original y predeterminado del firmware.
- **Español:** traducción al Español para menús, mensajes y pantallas.

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

Puedes activar o desactivar las carátulas desde el menú con **Mostrar carátulas**.

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

## Música del menú

El menú puede reproducir música de fondo mientras navegas. El archivo debe ser un **`.spc`** llamado `menu.spc` y estar en esta ruta:

```text
/sd2snes/menu.spc
```

Para añadir música:

1. Descarga un archivo `.spc`.
2. Renómbralo como `menu.spc`.
3. Cópialo en la carpeta `/sd2snes/` de la tarjeta SD.
4. Enciende la consola.

Buenos lugares para encontrar archivos `.spc`:

- [snesmusic.org](https://snesmusic.org)
- [zophar.net/music](https://www.zophar.net/music/nintendo-snes-spc)

Puedes activar o desactivar la música en **Configuración → Opciones del Navegador → Música del menú**.

> [!TIP]
> Algunas bandas sonoras vienen como archivos `.rsn`. Un `.rsn` suele ser un archivo comprimido con varios `.spc` dentro. Extrae el `.rsn` y elige uno de los archivos `.spc`.

## Reset al menú

El botón de reset puede llevarte de vuelta al menú de sd2snes en lugar de simplemente reiniciar el juego. Este fork añade dos opciones para facilitar el regreso a tu lista de juegos.

Configúralo en **Configuración → Opciones en Juego → Reset al menú**:

- **Des:** el reset se comporta como un reset normal del SNES.
- **Act:** un reset corto vuelve al menú.
- **Carpeta:** vuelve al menú y abre la carpeta del juego que estabas jugando.
- **ROM:** vuelve a la carpeta y resalta la ROM que estabas jugando.

Las opciones **Carpeta** y **ROM** funcionan después de un reset de vuelta al menú. Al encender la consola desde cero, sigue iniciando en el menú principal normal.

## Tema personalizado

Puedes personalizar el logo, la fuente, la paleta, el fondo y los colores del selector del menú.

Usa el editor de temas hecho para este fork:

### 👉 [sd2snes.ludufre.com/theme](https://sd2snes.ludufre.com/theme/)

> [!IMPORTANT]
> El editor oficial de temas de sd2snes no soporta el formato de tema de este fork. Usa el editor anterior al editar el `m3nu.bin` de este firmware.

Flujo básico para editar un tema:

1. Abre el editor de temas.
2. Sube tu `m3nu.bin`.
3. Sube un **PNG 128×56** si quieres cambiar el logo.
4. Elige las partes que quieres modificar.
5. Descarga el archivo editado y cópialo de nuevo a la tarjeta SD.

## Problemas comunes

**El menú no cambió después de la instalación.**

Comprueba que usaste la release **full** correspondiente y copiaste los archivos en la raíz de la tarjeta SD. Si usaste un paquete que no sea full, comprueba que el firmware oficial correspondiente se instaló primero y que los archivos de este fork se copiaron en `/sd2snes`.

**Las carátulas no aparecen.**

Comprueba que las carátulas estén activadas, que cada archivo `.cov` tenga el mismo nombre que su ROM y que las carátulas se hayan generado con **sd2snes-covers v1.1.0 o más reciente**.

**La música del menú no suena.**

Comprueba que el archivo se llame exactamente `menu.spc`, que esté en `/sd2snes/menu.spc` y que realmente sea un archivo `.spc`. MP3 y WAV no funcionan.

**Un parche no aparece.**

Comprueba que el parche esté en la misma carpeta que la ROM, que empiece con el nombre de la ROM y que termine en `.ips` o `.bps`.

## Notas avanzadas

Esta sección es principalmente para desarrolladores, mantenedores y usuarios avanzados. No la necesitas para una instalación normal.

### Formato de las carátulas

Este firmware, a partir de la versión **v1.11.2-br-2.1**, usa el formato `.cov` más nuevo con sprites OBJ. Las carátulas generadas por versiones antiguas de la aplicación de carátulas no se muestran correctamente. Vuelve a generarlas con **sd2snes-covers v1.1.0 o más reciente**.

### Verificación de integridad de parches BPS

La verificación de integridad para BPS se puede activar en **Configuración → Opciones de Patches → Verificar Integridad**.

Esta opción viene **Desactivada por defecto**. Cuando está activada, el firmware vuelve a leer la ROM después de aplicar un parche BPS para confirmar que se aplicó correctamente. Esto hace que la carga de BPS sea más lenta; por ejemplo, un parche BPS de 4 MB puede añadir unos 15 segundos a la carga, en promedio. Los parches IPS no son verificados por esta opción.

### Limitaciones de la música del menú

Solo se admiten archivos `.spc`. Un archivo `.spc` no es una grabación de audio común; es una instantánea del estado del chip de sonido del SNES y tiene un límite de 64 KB. No existe una conversión directa de MP3 a SPC.

Cuando la música se carga al iniciar, después de un reset o después de activar la opción, el menú puede pausarse brevemente mientras el archivo se envía al chip de sonido del SNES. Abrir un `.spc` desde el navegador de archivos pausa la música de fondo y la reanuda cuando vuelves con el botón B.

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

Contribuidores del repositorio original de sd2snes listados por GitHub:

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

### Código fuente y licencia

Este proyecto sigue licenciado bajo GNU General Public License v2.0 (GPL-2.0), siguiendo la licencia del proyecto original sd2snes.

Todos los derechos de autor originales pertenecen a sus respectivos autores y contribuidores.

Cambios específicos de este fork:
Copyright (C) 2026 Luan Freitas y contribuidores

El código fuente de todos los binarios/releases distribuidos está disponible en este repositorio y en las tags/releases correspondientes de Git, de acuerdo con los requisitos de la GPL.

Consulta el [README original](https://github.com/mrehkopf/sd2snes/blob/master/README.md).

Consulta el [README de FURiOUS](README.Savestates.FURiOUS.md) para información sobre Save States.
