const $ = s => document.querySelector(s);
let cwd = "/";
let devName = "";   // sd2snes device name from /api/ping (drives the OTA preselection)

/* ---- inline lucide-style icons (ported from the sd2snes+ Manager icon set) ---- */
const svg = p => '<svg class="ico" viewBox="0 0 24 24" fill="none" stroke="currentColor"'
  + ' stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">' + p + '</svg>';
const IC = {
  refresh:'<path d="M21 12a9 9 0 1 1-3-6.7"/><path d="M21 4v4h-4"/>',
  folderPlus:'<path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/><path d="M12 11v5M9.5 13.5h5"/>',
  upload:'<path d="M12 16V4m0 0-4 4m4-4 4 4"/><path d="M4 20h16"/>',
  download:'<path d="M12 3v12m0 0 4-4m-4 4-4-4"/><path d="M5 21h14"/>',
  wifi:'<path d="M12 20h.01"/><path d="M2 8.82a15 15 0 0 1 20 0"/><path d="M5 12.86a10 10 0 0 1 14 0"/><path d="M8.5 16.43a5 5 0 0 1 7 0"/>',
  folder:'<path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/>',
  file:'<path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><path d="M14 2v6h6"/>',
  lock:'<rect x="5" y="11" width="14" height="10" rx="2"/><path d="M8 11V7a4 4 0 0 1 8 0v4"/>',
  x:'<path d="M18 6 6 18M6 6l12 12"/>',
  grid:'<rect x="3" y="3" width="7" height="7" rx="1"/><rect x="14" y="3" width="7" height="7" rx="1"/><rect x="3" y="14" width="7" height="7" rx="1"/><rect x="14" y="14" width="7" height="7" rx="1"/>',
  save:'<path d="M5 3h11l3 3v15H5z"/><path d="M8 3v5h7M8 21v-7h8v7"/>',
  sliders:'<path d="M4 21v-7M4 10V3M12 21v-9M12 8V3M20 21v-5M20 12V3M1 14h6M9 8h6M17 16h6"/>',
  cpu:'<rect x="4" y="4" width="16" height="16" rx="2"/><rect x="9" y="9" width="6" height="6"/><path d="M9 1v3M15 1v3M9 20v3M15 20v3M20 9h3M20 14h3M1 9h3M1 14h3"/>',
  play:'<path d="M7 4v16l13-8z"/>',
  trash:'<path d="M4 7h16M9 7V5a1 1 0 0 1 1-1h4a1 1 0 0 1 1 1v2m2 0v12a1 1 0 0 1-1 1H8a1 1 0 0 1-1-1V7"/>',
  move:'<path d="M5 9l-3 3 3 3M9 5l3-3 3 3M15 19l-3 3-3-3M19 9l3 3-3 3M2 12h20M12 2v20"/>',
  checkSquare:'<path d="M9 11l3 3L22 4"/><path d="M21 12v7a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11"/>',
  broom:'<path d="M19 3l2 2M13 9l6-6M12 10l2 2M8 12l-3 8 8-3M6 14l4 4"/>',
  clock:'<circle cx="12" cy="12" r="9"/><path d="M12 8v4l3 2"/>',
  power:'<path d="M12 4v8M6.3 7a8 8 0 1 0 11.4 0"/>',
  sd:'<path d="M6 3h9l5 5v13a1 1 0 0 1-1 1H6a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1z"/><path d="M10 3v4M13 3v4M16 4v3"/>',
  check:'<path d="M20 6 9 17l-5-5"/>',
  image:'<rect x="3" y="3" width="18" height="18" rx="2"/><circle cx="9" cy="9" r="2"/><path d="m21 15-5-5L5 21"/>'
};

/* ---- i18n (language comes from the sd2snes: 0=EN, 1=PT-BR, 2=ES) ---- */
const LANGS = ["en", "pt", "es", "de"];
let lang = "en";
const I18N = {
  en: {
    refresh:"Refresh", newFolder:"New folder", upload:"Upload", wifi:"WiFi",
    dropHint:"Drag files here to upload", colName:"Name", colSize:"Size",
    loading:"Loading...", empty:"(empty folder)", items:"{0} item(s)",
    listErr:"List error: {0}", download:"download", rename:"rename", del:"delete",
    cfmDelete:'Delete "{0}"?', delFail:"Delete failed (status {0})",
    pNewName:"New name:", renFail:"Rename failed (status {0})",
    pNewFolder:"New folder name:", mkdirFail:"Create folder failed (status {0})",
    downloading:"Downloading", uploading:"Uploading",
    incomplete:"Incomplete: {0} of {1} - try again",
    saved:"saved", sent:"sent", done:"done", fail:"Failed: {0}", upFail:"Upload failed: {0}",
    wifiTitle:"WiFi network", forget:"Forget", scan:"Scan", close:"Close", cancel:"Cancel",
    connect:"Connect", password:"Password",
    connecting:"Connecting to {0}... (if the sd2snes AP drops, reconnect)",
    connected:"Connected: {0} ({1})", notConn:"Not connected - sd2snes AP only",
    stUnavail:"status unavailable", scanning:"Scanning networks...",
    noNets:"No networks found", scanFail:"Scan failed",
    connFail:"Could not connect to {0} - check password/signal",
    opening:"Connected: {0} ({1}) - opening {2}...",
    cfmForget:"Forget the saved network?", mcuNo:"MCU not responding",
    otaBtn:"Firmware", otaTitle:"Firmware update",
    otaPick:"Select what to write (replaces files on the SD):",
    otaWrite:"Write", otaNone:"Select at least one file",
    otaZipErr:"Could not read the ZIP: {0}", otaEmpty:"No files in the ZIP",
    otaWriting:"Writing ({0}/{1}) {2}", otaFail:"Failed on {0}: {1}",
    otaDoneMenu:"Done. Restart to load the new menu.",
    otaDonePower:"Done. Turn the console OFF and ON (a Reset is NOT enough) to apply the new firmware.",
    covTitle:"Cover art", nPalettes:"{0} palettes", covErr:"Could not read the cover",
    fwLoading:"Loading versions...", fwLoadErr:"Could not load versions: {0}",
    fwNone:"No releases found", fwUpdate:"Update", fwFull:"Full", fwLocal:"Local .zip...",
    fwDownloading:"Downloading firmware...",
    fwDlErr:"Download failed: {0} - the sd2snes.ludufre.com proxy must allow this origin (CORS).",
    fwNotesEmpty:"No release notes.",
    fwHow:"How do you want to update?", fwOnlineBtn:"Online (GitHub)", fwLocalBtn:"Local file (.zip)",
    fwChecking:"Checking connection...", fwOpenWifi:"Open WiFi",
    fwNeedWifi:"Connect to a WiFi network first to update online.",
    tabShelf:"Shelf", tabFiles:"Files", tabSaves:"Saves", tabSettings:"Settings", tabDevice:"Device",
    shelfContinue:"Continue", shelfFavorites:"Favorites", shelfRecents:"Recents", shelfRandom:"Random",
    shelfEmpty:"Your shelf is empty — play some games or add favorites.",
    shelfErr:"Could not read the lists.", wip:"Coming in a later phase.",
    savesTitle:"Save backup", savesIntro:"Back up your in-game saves (.srm) and save-states to your phone, or restore them onto a card.",
    savesScan:"Scan saves", savesScanning:"Scanning... {0} found", savesNone:"No saves found",
    savesBackup:"Backup", savesRestore:"Restore .zip", savesAll:"All",
    savesBackingUp:"Backing up {0}/{1}...", savesRestoring:"Restoring {0}/{1}...",
    savesBackupDone:"Backup ready", savesRestoreDone:"Restored {0} file(s)", savesPickNone:"Select at least one",
    select:"Select", selCount:"{0} selected", move:"Move", moveTo:"Move to folder:", cfmDeleteN:"Delete {0} item(s)?",
    setTitle:"Settings", setIntro:"Change the card's menu settings. Saved to config.yml.",
    setSave:"Save", setApply:"Save & apply now", setSaved:"Saved. Reset the console to apply.",
    setApplied:"Saved & applied.", setReadErr:"Could not read the config — reload and retry.", setSaveErr:"Could not save: {0}",
    setApplyErr:"Saved. Power-cycle the console once so it takes effect (the new firmware needs it).",
    devTitle:"Device", devMaint:"Maintenance", devBoot:"Boot health", devBootOk:"Card looks bootable.", devBootMiss:"Missing boot files: {0}",
    devAutoboot:"Auto-boot", devAutobootNone:"none", devAutobootClear:"Clear",
    playNext:"Play next boot", playNextDone:"Set — boots on next power-on (hold START to skip).",
    sweepTitle:"Orphan sweeper", sweepIntro:"Leftover .cov/.srm/.ips/.yml with no matching ROM.",
    sweepScan:"Scan", sweepScanning:"Scanning... {0} orphans", sweepNone:"No orphans found", sweepDel:"Delete", sweepDone:"Deleted {0}",
    auditTitle:"Library audit", auditScan:"Scan library", auditScanning:"Scanning... {0} ROMs"
  },
  pt: {
    refresh:"Atualizar", newFolder:"Nova pasta", upload:"Enviar", wifi:"WiFi",
    dropHint:"Arraste arquivos aqui para enviar", colName:"Nome", colSize:"Tamanho",
    loading:"Carregando...", empty:"(pasta vazia)", items:"{0} item(ns)",
    listErr:"Erro ao listar: {0}", download:"baixar", rename:"renomear", del:"apagar",
    cfmDelete:'Apagar "{0}"?', delFail:"Falha ao apagar (status {0})",
    pNewName:"Novo nome:", renFail:"Falha ao renomear (status {0})",
    pNewFolder:"Nome da nova pasta:", mkdirFail:"Falha ao criar pasta (status {0})",
    downloading:"Baixando", uploading:"Enviando",
    incomplete:"Incompleto: {0} de {1} - tente de novo",
    saved:"salvo", sent:"enviado", done:"concluído", fail:"Falha: {0}", upFail:"Falha ao enviar: {0}",
    wifiTitle:"Rede WiFi", forget:"Esquecer", scan:"Procurar", close:"Fechar", cancel:"Cancelar",
    connect:"Conectar", password:"Senha",
    connecting:"Conectando a {0}... (se o AP sd2snes cair, reconecte)",
    connected:"Conectado: {0} ({1})", notConn:"Não conectado - somente o AP sd2snes",
    stUnavail:"status indisponível", scanning:"Procurando redes...",
    noNets:"Nenhuma rede encontrada", scanFail:"Falha ao procurar",
    connFail:"Não conectou a {0} - verifique a senha/sinal",
    opening:"Conectado: {0} ({1}) - abrindo {2}...",
    cfmForget:"Esquecer a rede salva?", mcuNo:"MCU sem resposta",
    otaBtn:"Firmware", otaTitle:"Atualizar firmware",
    otaPick:"Selecione o que gravar (substitui arquivos no SD):",
    otaWrite:"Gravar", otaNone:"Selecione ao menos um arquivo",
    otaZipErr:"Não foi possível ler o ZIP: {0}", otaEmpty:"Nenhum arquivo no ZIP",
    otaWriting:"Gravando ({0}/{1}) {2}", otaFail:"Falha em {0}: {1}",
    otaDoneMenu:"Concluído. Reinicie para carregar o novo menu.",
    otaDonePower:"Concluído. Desligue e ligue o console (um Reset NÃO basta) para aplicar o novo firmware.",
    covTitle:"Capa", nPalettes:"{0} paletas", covErr:"Não foi possível ler a capa",
    fwLoading:"Carregando versões...", fwLoadErr:"Não foi possível carregar as versões: {0}",
    fwNone:"Nenhuma versão encontrada", fwUpdate:"Atualização", fwFull:"Completo", fwLocal:"Arquivo .zip...",
    fwDownloading:"Baixando firmware...",
    fwDlErr:"Falha no download: {0} - o proxy sd2snes.ludufre.com precisa liberar esta origem (CORS).",
    fwNotesEmpty:"Sem notas de versão.",
    fwHow:"Como deseja atualizar?", fwOnlineBtn:"Online (GitHub)", fwLocalBtn:"Arquivo local (.zip)",
    fwChecking:"Verificando conexão...", fwOpenWifi:"Abrir WiFi",
    fwNeedWifi:"Conecte-se a uma rede WiFi primeiro para atualizar online.",
    tabShelf:"Estante", tabFiles:"Arquivos", tabSaves:"Saves", tabSettings:"Ajustes", tabDevice:"Dispositivo",
    shelfContinue:"Continuar", shelfFavorites:"Favoritos", shelfRecents:"Recentes", shelfRandom:"Aleatório",
    shelfEmpty:"Sua estante está vazia — jogue alguns jogos ou marque favoritos.",
    shelfErr:"Não foi possível ler as listas.", wip:"Em breve (próxima fase).",
    savesTitle:"Backup de saves", savesIntro:"Faça backup dos seus saves (.srm) e save-states no celular, ou restaure-os num cartão.",
    savesScan:"Escanear saves", savesScanning:"Escaneando... {0} encontrados", savesNone:"Nenhum save encontrado",
    savesBackup:"Backup", savesRestore:"Restaurar .zip", savesAll:"Todos",
    savesBackingUp:"Backup {0}/{1}...", savesRestoring:"Restaurando {0}/{1}...",
    savesBackupDone:"Backup pronto", savesRestoreDone:"{0} arquivo(s) restaurado(s)", savesPickNone:"Selecione ao menos um",
    select:"Selecionar", selCount:"{0} selecionados", move:"Mover", moveTo:"Mover para a pasta:", cfmDeleteN:"Apagar {0} item(ns)?",
    setTitle:"Ajustes", setIntro:"Mude os ajustes do menu no cartão. Salvo em config.yml.",
    setSave:"Salvar", setApply:"Salvar e aplicar agora", setSaved:"Salvo. Dê reset no console para aplicar.",
    setApplied:"Salvo e aplicado.", setReadErr:"Não consegui ler a config — recarregue e tente de novo.", setSaveErr:"Não foi possível salvar: {0}",
    setApplyErr:"Salvo. Desligue e ligue o console uma vez pra valer (o firmware novo precisa disso).",
    devTitle:"Dispositivo", devMaint:"Manutenção", devBoot:"Saúde de boot", devBootOk:"O cartão parece bootável.", devBootMiss:"Arquivos de boot faltando: {0}",
    devAutoboot:"Auto-boot", devAutobootNone:"nenhum", devAutobootClear:"Limpar",
    playNext:"Tocar no próximo boot", playNextDone:"Definido — boota no próximo power-on (segure START pra pular).",
    sweepTitle:"Faxina de órfãos", sweepIntro:".cov/.srm/.ips/.yml sobrando sem ROM correspondente.",
    sweepScan:"Escanear", sweepScanning:"Escaneando... {0} órfãos", sweepNone:"Nenhum órfão", sweepDel:"Apagar", sweepDone:"{0} apagados",
    auditTitle:"Auditoria da biblioteca", auditScan:"Escanear biblioteca", auditScanning:"Escaneando... {0} ROMs"
  },
  es: {
    refresh:"Actualizar", newFolder:"Nueva carpeta", upload:"Subir", wifi:"WiFi",
    dropHint:"Arrastra archivos aquí para subir", colName:"Nombre", colSize:"Tamaño",
    loading:"Cargando...", empty:"(carpeta vacía)", items:"{0} elemento(s)",
    listErr:"Error al listar: {0}", download:"descargar", rename:"renombrar", del:"borrar",
    cfmDelete:'¿Borrar "{0}"?', delFail:"Error al borrar (estado {0})",
    pNewName:"Nuevo nombre:", renFail:"Error al renombrar (estado {0})",
    pNewFolder:"Nombre de la nueva carpeta:", mkdirFail:"Error al crear carpeta (estado {0})",
    downloading:"Descargando", uploading:"Subiendo",
    incomplete:"Incompleto: {0} de {1} - inténtalo de nuevo",
    saved:"guardado", sent:"subido", done:"completado", fail:"Error: {0}", upFail:"Error al subir: {0}",
    wifiTitle:"Red WiFi", forget:"Olvidar", scan:"Buscar", close:"Cerrar", cancel:"Cancelar",
    connect:"Conectar", password:"Contraseña",
    connecting:"Conectando a {0}... (si cae el AP sd2snes, reconéctate)",
    connected:"Conectado: {0} ({1})", notConn:"No conectado - solo el AP sd2snes",
    stUnavail:"estado no disponible", scanning:"Buscando redes...",
    noNets:"No se encontraron redes", scanFail:"Error al buscar",
    connFail:"No se pudo conectar a {0} - revisa contraseña/señal",
    opening:"Conectado: {0} ({1}) - abriendo {2}...",
    cfmForget:"¿Olvidar la red guardada?", mcuNo:"MCU sin respuesta",
    otaBtn:"Firmware", otaTitle:"Actualizar firmware",
    otaPick:"Selecciona qué grabar (reemplaza archivos en la SD):",
    otaWrite:"Grabar", otaNone:"Selecciona al menos un archivo",
    otaZipErr:"No se pudo leer el ZIP: {0}", otaEmpty:"No hay archivos en el ZIP",
    otaWriting:"Grabando ({0}/{1}) {2}", otaFail:"Error en {0}: {1}",
    otaDoneMenu:"Hecho. Reinicia para cargar el nuevo menú.",
    otaDonePower:"Hecho. Apaga y enciende la consola (un Reset NO basta) para aplicar el nuevo firmware.",
    covTitle:"Portada", nPalettes:"{0} paletas", covErr:"No se pudo leer la portada",
    fwLoading:"Cargando versiones...", fwLoadErr:"No se pudieron cargar las versiones: {0}",
    fwNone:"No se encontraron versiones", fwUpdate:"Actualización", fwFull:"Completo", fwLocal:"Archivo .zip...",
    fwDownloading:"Descargando firmware...",
    fwDlErr:"Error de descarga: {0} - el proxy sd2snes.ludufre.com debe permitir este origen (CORS).",
    fwNotesEmpty:"Sin notas.",
    fwHow:"¿Cómo deseas actualizar?", fwOnlineBtn:"En línea (GitHub)", fwLocalBtn:"Archivo local (.zip)",
    fwChecking:"Verificando conexión...", fwOpenWifi:"Abrir WiFi",
    fwNeedWifi:"Conéctate a una red WiFi primero para actualizar en línea.",
    tabShelf:"Estante", tabFiles:"Archivos", tabSaves:"Guardados", tabSettings:"Ajustes", tabDevice:"Dispositivo",
    shelfContinue:"Continuar", shelfFavorites:"Favoritos", shelfRecents:"Recientes", shelfRandom:"Aleatorio",
    shelfEmpty:"Tu estante está vacía — juega o marca favoritos.",
    shelfErr:"No se pudieron leer las listas.", wip:"Próximamente.",
    savesTitle:"Copia de guardados", savesIntro:"Respalda tus guardados (.srm) y save-states en el teléfono, o restáuralos en una tarjeta.",
    savesScan:"Escanear guardados", savesScanning:"Escaneando... {0} encontrados", savesNone:"No se encontraron guardados",
    savesBackup:"Respaldar", savesRestore:"Restaurar .zip", savesAll:"Todos",
    savesBackingUp:"Respaldando {0}/{1}...", savesRestoring:"Restaurando {0}/{1}...",
    savesBackupDone:"Copia lista", savesRestoreDone:"{0} archivo(s) restaurado(s)", savesPickNone:"Selecciona al menos uno",
    select:"Seleccionar", selCount:"{0} seleccionados", move:"Mover", moveTo:"Mover a la carpeta:", cfmDeleteN:"¿Borrar {0} elemento(s)?",
    setTitle:"Ajustes", setIntro:"Cambia los ajustes del menú en la tarjeta. Guardado en config.yml.",
    setSave:"Guardar", setApply:"Guardar y aplicar", setSaved:"Guardado. Reinicia la consola para aplicar.",
    setApplied:"Guardado y aplicado.", setReadErr:"No se pudo leer la config — recarga y reintenta.", setSaveErr:"No se pudo guardar: {0}",
    setApplyErr:"Guardado. Apaga y enciende la consola una vez para que tome efecto (el firmware nuevo lo necesita).",
    devTitle:"Dispositivo", devMaint:"Mantenimiento", devBoot:"Salud de arranque", devBootOk:"La tarjeta parece arrancable.", devBootMiss:"Faltan archivos de arranque: {0}",
    devAutoboot:"Auto-arranque", devAutobootNone:"ninguno", devAutobootClear:"Limpiar",
    playNext:"Jugar al próximo arranque", playNextDone:"Listo — arranca al encender (mantén START para omitir).",
    sweepTitle:"Limpieza de huérfanos", sweepIntro:".cov/.srm/.ips/.yml sin ROM correspondiente.",
    sweepScan:"Escanear", sweepScanning:"Escaneando... {0} huérfanos", sweepNone:"Sin huérfanos", sweepDel:"Borrar", sweepDone:"{0} borrados",
    auditTitle:"Auditoría de biblioteca", auditScan:"Escanear biblioteca", auditScanning:"Escaneando... {0} ROMs"
  },
  de: {
    refresh:"Aktualisieren", newFolder:"Neuer Ordner", upload:"Hochladen", wifi:"WLAN",
    dropHint:"Dateien zum Hochladen hierher ziehen", colName:"Name", colSize:"Größe",
    loading:"Lädt...", empty:"(leerer Ordner)", items:"{0} Element(e)",
    listErr:"Listenfehler: {0}", download:"laden", rename:"umbenennen", del:"löschen",
    cfmDelete:'"{0}" löschen?', delFail:"Löschen fehlgeschlagen (Status {0})",
    pNewName:"Neuer Name:", renFail:"Umbenennen fehlgeschlagen (Status {0})",
    pNewFolder:"Name des neuen Ordners:", mkdirFail:"Ordner erstellen fehlgeschlagen (Status {0})",
    downloading:"Lädt herunter", uploading:"Lädt hoch",
    incomplete:"Unvollständig: {0} von {1} - erneut versuchen",
    saved:"gespeichert", sent:"gesendet", done:"fertig", fail:"Fehlgeschlagen: {0}", upFail:"Upload fehlgeschlagen: {0}",
    wifiTitle:"WLAN-Netzwerk", forget:"Vergessen", scan:"Suchen", close:"Schließen", cancel:"Abbrechen",
    connect:"Verbinden", password:"Passwort",
    connecting:"Verbinde mit {0}... (falls der sd2snes-AP abbricht, neu verbinden)",
    connected:"Verbunden: {0} ({1})", notConn:"Nicht verbunden - nur sd2snes-AP",
    stUnavail:"Status nicht verfügbar", scanning:"Suche Netzwerke...",
    noNets:"Keine Netzwerke gefunden", scanFail:"Suche fehlgeschlagen",
    connFail:"Verbindung zu {0} fehlgeschlagen - Passwort/Signal prüfen",
    opening:"Verbunden: {0} ({1}) - öffne {2}...",
    cfmForget:"Gespeichertes Netzwerk vergessen?", mcuNo:"MCU antwortet nicht",
    otaBtn:"Firmware", otaTitle:"Firmware-Update",
    otaPick:"Wählen, was geschrieben wird (ersetzt Dateien auf der SD):",
    otaWrite:"Schreiben", otaNone:"Mindestens eine Datei wählen",
    otaZipErr:"ZIP konnte nicht gelesen werden: {0}", otaEmpty:"Keine Dateien im ZIP",
    otaWriting:"Schreibe ({0}/{1}) {2}", otaFail:"Fehler bei {0}: {1}",
    otaDoneMenu:"Fertig. Neustarten, um das neue Menü zu laden.",
    otaDonePower:"Fertig. Konsole AUS und AN schalten (ein Reset reicht NICHT), um die neue Firmware zu übernehmen.",
    covTitle:"Cover", nPalettes:"{0} Paletten", covErr:"Cover konnte nicht gelesen werden",
    fwLoading:"Lade Versionen...", fwLoadErr:"Versionen konnten nicht geladen werden: {0}",
    fwNone:"Keine Releases gefunden", fwUpdate:"Update", fwFull:"Komplett", fwLocal:"Lokale .zip...",
    fwDownloading:"Lade Firmware herunter...",
    fwDlErr:"Download fehlgeschlagen: {0} - der Proxy sd2snes.ludufre.com muss diese Herkunft erlauben (CORS).",
    fwNotesEmpty:"Keine Release-Notizen.",
    fwHow:"Wie möchtest du aktualisieren?", fwOnlineBtn:"Online (GitHub)", fwLocalBtn:"Lokale Datei (.zip)",
    fwChecking:"Prüfe Verbindung...", fwOpenWifi:"WLAN öffnen",
    fwNeedWifi:"Zuerst mit einem WLAN verbinden, um online zu aktualisieren.",
    tabShelf:"Regal", tabFiles:"Dateien", tabSaves:"Speicher", tabSettings:"Einstellungen", tabDevice:"Gerät",
    shelfContinue:"Fortsetzen", shelfFavorites:"Favoriten", shelfRecents:"Zuletzt", shelfRandom:"Zufällig",
    shelfEmpty:"Dein Regal ist leer — spiele ein paar Spiele oder füge Favoriten hinzu.",
    shelfErr:"Listen konnten nicht gelesen werden.", wip:"Kommt in einer späteren Phase.",
    savesTitle:"Speicher-Backup", savesIntro:"Sichere deine Spielstände (.srm) und Savestates aufs Handy oder stelle sie auf eine Karte wieder her.",
    savesScan:"Speicher scannen", savesScanning:"Scanne... {0} gefunden", savesNone:"Keine Speicherstände gefunden",
    savesBackup:"Backup", savesRestore:".zip wiederherstellen", savesAll:"Alle",
    savesBackingUp:"Sichere {0}/{1}...", savesRestoring:"Stelle wieder her {0}/{1}...",
    savesBackupDone:"Backup bereit", savesRestoreDone:"{0} Datei(en) wiederhergestellt", savesPickNone:"Mindestens eine wählen",
    select:"Auswählen", selCount:"{0} ausgewählt", move:"Verschieben", moveTo:"In Ordner verschieben:", cfmDeleteN:"{0} Element(e) löschen?",
    setTitle:"Einstellungen", setIntro:"Menü-Einstellungen der Karte ändern. In config.yml gespeichert.",
    setSave:"Speichern", setApply:"Speichern & jetzt anwenden", setSaved:"Gespeichert. Konsole zurücksetzen, um zu übernehmen.",
    setApplied:"Gespeichert & angewendet.", setReadErr:"Konfiguration nicht lesbar — neu laden und erneut versuchen.", setSaveErr:"Speichern fehlgeschlagen: {0}",
    setApplyErr:"Gespeichert. Konsole einmal aus- und einschalten, damit es wirkt (die neue Firmware braucht das).",
    devTitle:"Gerät", devMaint:"Wartung", devBoot:"Boot-Zustand", devBootOk:"Karte scheint bootfähig.", devBootMiss:"Fehlende Boot-Dateien: {0}",
    devAutoboot:"Auto-Boot", devAutobootNone:"keiner", devAutobootClear:"Löschen",
    playNext:"Nächsten Start spielen", playNextDone:"Gesetzt — startet beim nächsten Einschalten (START halten zum Überspringen).",
    sweepTitle:"Verwaiste Dateien", sweepIntro:"Übrige .cov/.srm/.ips/.yml ohne passende ROM.",
    sweepScan:"Scannen", sweepScanning:"Scanne... {0} verwaist", sweepNone:"Keine verwaisten Dateien", sweepDel:"Löschen", sweepDone:"{0} gelöscht",
    auditTitle:"Bibliotheks-Prüfung", auditScan:"Bibliothek scannen", auditScanning:"Scanne... {0} ROMs"
  }
};
function t(k){
  let s = (I18N[lang] && I18N[lang][k]) || I18N.en[k] || k;
  for (let i = 1; i < arguments.length; i++) s = s.replace("{" + (i - 1) + "}", arguments[i]);
  return s;
}
function applyI18n(){
  document.documentElement.lang = (lang === "pt") ? "pt-br" : lang;
  const lbl = (el, ic, k) => { $(el).innerHTML = svg(ic) + "<span></span>"; $(el).lastChild.textContent = t(k); };
  lbl("#b_select", IC.checkSquare,"select");
  lbl("#b_reload", IC.refresh,    "refresh");
  lbl("#b_mkdir",  IC.folderPlus, "newFolder");
  lbl("#b_upload", IC.upload,     "upload");
  $("#drop").textContent     = t("dropHint");
  $("#b_cname").textContent  = t("colName");
  $("#b_csize").textContent  = t("colSize");
  $("#b_wtitle").textContent = t("wifiTitle");
  $("#wforget").textContent  = t("forget");
  $("#wscan").textContent    = t("scan");
  $("#b_wclose").textContent = t("close");
  lbl("#b_fwlocal", IC.file, "fwLocal");
  $("#b_fwclose").innerHTML  = svg(IC.x);
  $("#fwdest").textContent   = "→ /sd2snes";
  lbl("#fwpickOnline", IC.download, "fwOnlineBtn");
  lbl("#fwpickLocal",  IC.file,     "fwLocalBtn");
  [["shelf",IC.grid,"tabShelf"],["files",IC.folder,"tabFiles"],["saves",IC.save,"tabSaves"],
   ["settings",IC.sliders,"tabSettings"],["device",IC.cpu,"tabDevice"]].forEach(([tk,ic,k])=>{
    const b=document.querySelector('.tabs [data-tab="'+tk+'"]');
    if(b){ b.innerHTML=svg(ic)+"<span></span>"; b.lastChild.textContent=t(k); } });
}

const status = m => { $("#status").textContent = m || ""; };
function human(n){ if(n<1024)return n+" B"; const u=["KB","MB","GB"]; let i=-1;
  do{ n/=1024; i++; }while(n>=1024 && i<2); return n.toFixed(n<10?1:0)+" "+u[i]; }
function join(dir, name){ return (dir==="/"?"":dir)+"/"+name; }

function crumbs(){
  const c=$("#crumbs"); c.innerHTML="";
  const root=document.createElement("a"); root.textContent="SD"; root.onclick=()=>go("/"); c.appendChild(root);
  let acc="";
  cwd.split("/").filter(Boolean).forEach(p=>{ acc+="/"+p; const seg=acc;
    const s=document.createElement("span"); s.className="sep"; s.textContent="/"; c.appendChild(s);
    const a=document.createElement("a"); a.textContent=p; a.onclick=()=>go(seg); c.appendChild(a); });
}

function parent(p){ p=(p||"/").replace(/\/+$/,""); const i=p.lastIndexOf("/"); return i<=0?"/":p.slice(0,i); }
function rowUp(tb){
  const tr=document.createElement("tr"), nm=document.createElement("td");
  nm.innerHTML='<span class="name dir"><span class="ic">↑</span><span class="lbl">..</span></span>';
  nm.querySelector(".name").onclick=()=>go(parent(cwd));
  const sz=document.createElement("td"); sz.className="size";
  const ac=document.createElement("td"); ac.className="act";
  tr.append(nm,sz,ac); tb.appendChild(tr);
}
const HIDE = new Set([".", "..", ".DS_Store", ".fseventsd", ".Spotlight-V100",
  ".TemporaryItems", ".Trashes", "Thumbs.db", "System Volume Information",
  "$RECYCLE.BIN", "desktop.ini"]);
const hidden = n => HIDE.has(n) || n.startsWith("._");

async function go(path){
  cwd=path||"/"; crumbs(); status(t("loading"));
  const tb=$("#list");
  tb.innerHTML='<tr><td colspan="3" class="empty"><span class="spin"></span>'+t("loading")+'</td></tr>';
  try{
    const r=await fetch("/api/ls?path="+encodeURIComponent(cwd));
    if(!r.ok) throw new Error("HTTP "+r.status);
    const j=await r.json();
    const ents=(j.entries||[]).filter(e=>!hidden(e.name))
      .sort((a,b)=>(b.dir-a.dir)||a.name.localeCompare(b.name,undefined,{numeric:true}));
    tb.innerHTML="";
    if(cwd!=="/") rowUp(tb);
    const covThumbs=[];
    for(const e of ents){
      const full=join(cwd,e.name);
      const isc = !e.dir && isCov(e.name);
      const tr=document.createElement("tr");
      tr.dataset.full=full; tr._ent=e;               // batch-select target
      const nm=document.createElement("td");
      nm.innerHTML='<span class="name'+(e.dir?' dir':isc?' cov':'')+'"><span class="ic">'+svg(e.dir?IC.folder:IC.file)+'</span><span class="lbl"></span></span>';
      nm.querySelector(".lbl").textContent=e.name;
      const nameEl=nm.querySelector(".name");
      if(e.dir) nameEl.onclick=()=>{ if(selMode) return; go(full); };
      else if(isc){ nameEl.onclick=()=>{ if(selMode) return; covPreview(full,e.name); };
        const ic=nm.querySelector(".ic"); ic.dataset.cov=full; covThumbs.push(ic); }
      const sz=document.createElement("td"); sz.className="size"; sz.textContent=e.dir?"-":human(e.size);
      const ac=document.createElement("td"); ac.className="act";
      if(!e.dir){ const d=document.createElement("a"); d.textContent=t("download"); d.className="always";
        d.onclick=()=>download(full,e.name,e.size); ac.appendChild(d); }
      const rn=document.createElement("a"); rn.textContent=t("rename"); rn.onclick=()=>rename(full,e.name); ac.appendChild(rn);
      const rm=document.createElement("a"); rm.className="danger"; rm.textContent=t("del"); rm.onclick=()=>del(full,e.name); ac.appendChild(rm);
      tr.append(nm,sz,ac); tb.appendChild(tr);
    }
    if(!ents.length){ const tr=document.createElement("tr");
      tr.innerHTML='<td colspan="3" class="empty">'+t("empty")+'</td>'; tb.appendChild(tr); }
    covObserve(covThumbs);
    status(t("items", ents.length));
  }catch(err){ $("#list").innerHTML=""; status(t("listErr", err.message)); }
}
const reload=()=>go(cwd);

async function del(full,name){ if(!confirm(t("cfmDelete",name)))return;
  const j=await(await fetch("/api/rm?path="+encodeURIComponent(full),{method:"POST"})).json();
  if(!j.ok) status(t("delFail",j.status)); reload(); }
async function rename(full,name){ const nn=prompt(t("pNewName"),name); if(!nn||nn===name)return;
  const j=await(await fetch("/api/mv?from="+encodeURIComponent(full)+"&to="+encodeURIComponent(join(cwd,nn)),{method:"POST"})).json();
  if(!j.ok) status(t("renFail",j.status)); reload(); }
async function mkdir(){ const nn=prompt(t("pNewFolder")); if(!nn)return;
  const j=await(await fetch("/api/mkdir?path="+encodeURIComponent(join(cwd,nn)),{method:"POST"})).json();
  if(!j.ok) status(t("mkdirFail",j.status)); reload(); }

/* ---- batch selection (Files): pick many, then bulk delete / move ---- */
let selMode=false; const selSet=new Map();
function toggleSel(on){ selMode = (on===true||on===false)? on : !selMode;
  $("#tab-files").classList.toggle("sel", selMode);
  $("#b_select").classList.toggle("on", selMode);
  if(!selMode){ selSet.clear(); document.querySelectorAll("#list tr.picked").forEach(r=>r.classList.remove("picked")); }
  updateSelBar();
}
function updateSelBar(){
  const bar=$("#selbar"); if(!selMode){ bar.classList.remove("show"); bar.innerHTML=""; return; }
  bar.classList.add("show"); bar.innerHTML="";
  const cnt=document.createElement("span"); cnt.className="selcnt"; cnt.textContent=t("selCount",selSet.size);
  const del=document.createElement("button"); del.className="danger"; del.innerHTML=svg(IC.trash)+"<span></span>";
  del.lastChild.textContent=t("del"); del.disabled=!selSet.size; del.onclick=bulkDelete;
  const mv=document.createElement("button"); mv.innerHTML=svg(IC.move)+"<span></span>";
  mv.lastChild.textContent=t("move"); mv.disabled=!selSet.size; mv.onclick=bulkMove;
  const cx=document.createElement("button"); cx.textContent=t("cancel"); cx.onclick=()=>toggleSel(false);
  bar.append(cnt,del,mv,cx);
}
$("#list").addEventListener("click", ev=>{
  if(!selMode) return;
  const tr=ev.target.closest("tr"); if(!tr||!tr.dataset.full) return;
  const full=tr.dataset.full;
  if(selSet.has(full)){ selSet.delete(full); tr.classList.remove("picked"); }
  else { selSet.set(full, tr._ent); tr.classList.add("picked"); }
  updateSelBar();
});
async function bulkDelete(){
  const items=[...selSet.keys()]; if(!items.length) return;
  if(!confirm(t("cfmDeleteN",items.length))) return;
  $("#selbar").querySelectorAll("button").forEach(b=>b.disabled=true);
  for(const full of items){ try{ await fetch("/api/rm?path="+encodeURIComponent(full),{method:"POST"}); }catch{} }
  toggleSel(false); reload();
}
async function bulkMove(){
  const items=[...selSet.entries()]; if(!items.length) return;
  const dest=prompt(t("moveTo"), cwd); if(!dest) return;
  const base=dest.replace(/\/+$/,"")||"/";
  $("#selbar").querySelectorAll("button").forEach(b=>b.disabled=true);
  for(const [full,e] of items){ const to=join(base, e.name);
    try{ await fetch("/api/mv?from="+encodeURIComponent(full)+"&to="+encodeURIComponent(to),{method:"POST"}); }catch{} }
  toggleSel(false); reload();
}

/* ---- cover-art (.cov) preview: decode the on-card box art in the browser
        (port of the sd2snes+ Manager lib/cov.js decoder). A .cov v4 is a grid of
        16x16 4bpp OBJ sprites: HEADER(12) + n_pal x 16 BGR555 palettes + BLOCKMAP
        (1 palette idx / 16x16 block) + (2*hSpr) rows x 16 cols of 8x8 planar tiles.
        We fetch it via /api/dl, decode to a canvas and render a PNG (index 0 =
        transparent letterbox). All fail-safe: any error keeps the plain file icon. ---- */
const isCov = n => /\.cov$/i.test(n);
function bgr555(w){ let r=(w&0x1f)<<3, g=((w>>5)&0x1f)<<3, b=((w>>10)&0x1f)<<3;
  r|=r>>5; g|=g>>5; b|=b>>5; return [r,g,b]; }
function decodeCov(b){                                   // Uint8Array -> {url,w,h,nPal}
  if(b.length<12 || b[0]!==0x43 || b[1]!==0x56 || b[2]!==4 || b[8]!==4) throw new Error("not .cov v4");
  const wSpr=b[4], hSpr=b[5], nPal=b[6];
  const palSz=nPal*32, bmSz=wSpr*hSpr, tlSz=2*hSpr*16*32;
  let off=12; if(!wSpr||!hSpr||!nPal || b.length<off+palSz+bmSz+tlSz) throw new Error("bad .cov");
  const pals=[];
  for(let p=0;p<nPal;p++){ const pal=[];
    for(let i=0;i<16;i++){ pal.push(bgr555(b[off]|(b[off+1]<<8))); off+=2; } pals.push(pal); }
  const bm=b.subarray(off,off+bmSz); off+=bmSz;
  const rows=2*hSpr;
  const tiles=Array.from({length:rows*8},()=>new Uint8Array(128));
  for(let cy=0;cy<rows;cy++) for(let cx=0;cx<16;cx++){
    const tt=b.subarray(off,off+32); off+=32;
    for(let r=0;r<8;r++){ const p0=tt[r*2],p1=tt[r*2+1],p2=tt[16+r*2],p3=tt[16+r*2+1];
      for(let c=0;c<8;c++){ const bit=7-c;
        tiles[cy*8+r][cx*8+c]=((p0>>bit)&1)|(((p1>>bit)&1)<<1)|(((p2>>bit)&1)<<2)|(((p3>>bit)&1)<<3); } }
  }
  const W=wSpr*16, H=hSpr*16;
  const cv=document.createElement("canvas"); cv.width=W; cv.height=H;
  const ctx=cv.getContext("2d"); const im=ctx.createImageData(W,H); const dt=im.data;
  for(let sy=0;sy<hSpr;sy++) for(let sx=0;sx<wSpr;sx++){
    let pi=bm[sy*wSpr+sx]; if(pi>=nPal) pi=0; const pal=pals[pi];
    for(let dy=0;dy<16;dy++) for(let dx=0;dx<16;dx++){
      const v=tiles[(2*sy+(dy>>3))*8+(dy&7)][(2*sx+(dx>>3))*8+(dx&7)];
      const p=((sy*16+dy)*W+(sx*16+dx))*4;
      if(v===0){ dt[p+3]=0; continue; }
      const c=pal[v]; dt[p]=c[0]; dt[p+1]=c[1]; dt[p+2]=c[2]; dt[p+3]=255;
    }
  }
  ctx.putImageData(im,0,0);
  return { url:cv.toDataURL("image/png"), w:W, h:H, nPal };
}
const covCache=new Map();       // path -> {url,w,h,nPal} | "err"
async function covLoad(full){
  const c=covCache.get(full);
  if(c){ if(c==="err") throw new Error("err"); return c; }
  try{
    const r=await fetch("/api/dl?path="+encodeURIComponent(full));
    if(!r.ok) throw new Error("HTTP "+r.status);
    const res=decodeCov(new Uint8Array(await r.arrayBuffer()));
    covCache.set(full,res); return res;
  }catch(err){ covCache.set(full,"err"); throw err; }
}
/* the ESP HTTP server is single-threaded, so stream/decode covers one at a time */
let covQ=[], covBusy=false;
function covEnqueue(job){ covQ.push(job); covPump(); }
async function covPump(){ if(covBusy) return; const job=covQ.shift(); if(!job) return;
  covBusy=true; try{ await job(); }catch{} covBusy=false; covPump(); }
let covObs=null;
function covObserve(ics){                                // lazy: decode only when visible
  if(covObs){ covObs.disconnect(); covObs=null; }
  if(!ics.length) return;
  covObs=new IntersectionObserver((es,obs)=>{ for(const e of es){ if(!e.isIntersecting) continue;
    obs.unobserve(e.target); const ic=e.target; covEnqueue(()=>covThumb(ic)); } }, {rootMargin:"150px"});
  ics.forEach(ic=>covObs.observe(ic));
}
async function covThumb(ic){
  try{ const res=await covLoad(ic.dataset.cov);
    const img=new Image(); img.alt=""; img.src=res.url;
    ic.classList.add("thumb"); ic.replaceChildren(img);
  }catch{ /* keep the generic file icon */ }
}
function covClose(){ $("#cov").classList.remove("show"); }
async function covPreview(full,name,rom,patch){
  $("#covTitle").textContent=t("covTitle"); $("#covName").textContent=name;
  $("#covMeta").textContent=""; $("#covDl").textContent=t("download"); $("#covClose").textContent=t("close");
  $("#covDl").onclick=()=>{ covClose(); download(full,name,0); };
  const old=$("#covPlay"); if(old) old.remove();
  if(rom){ const pn=document.createElement("button"); pn.id="covPlay"; pn.className="primary"; pn.style.marginRight="auto";
    pn.innerHTML=svg(IC.play)+"<span></span>"; pn.lastChild.textContent=t("playNext");
    pn.onclick=async()=>{ try{ await playNext(rom,patch); pn.disabled=true; $("#covMeta").textContent=t("playNextDone"); }catch{} };
    $("#cov .row").prepend(pn); }
  $("#covView").innerHTML='<span class="spin"></span>';
  $("#cov").classList.add("show");
  try{ const res=await covLoad(full);
    const img=new Image(); img.className="covbig"; img.src=res.url;
    $("#covView").replaceChildren(img);
    $("#covMeta").textContent=res.w+"×"+res.h+" · "+t("nPalettes",res.nPal);
  }catch{ $("#covView").innerHTML='<div class="coverr">'+t("covErr")+'</div>'; }
}

/* ---- progress modal (download + upload, baud-limited) ---- */
let ovCancel=null;
function ovOpen(title,name){ $("#ovTitle").textContent=title; $("#ovName").textContent=name;
  $("#ovErr").textContent=""; $("#ovBar").style.width="0"; $("#ovPct").textContent="0%";
  $("#ovInfo").textContent=""; $("#ovBtn").textContent=t("cancel"); $("#ov").classList.add("show"); }
function ovClose(){ $("#ov").classList.remove("show"); ovCancel=null; }
function ovProg(loaded,total,secs){
  const pct = total? Math.min(100, loaded/total*100) : 0;
  $("#ovBar").style.width=pct+"%"; $("#ovPct").textContent=(total?pct.toFixed(0):"-")+"%";
  const sp = secs>0? loaded/secs : 0;
  const eta = (sp>0&&total)? (total-loaded)/sp : 0;
  $("#ovInfo").textContent=human(loaded)+(total?" / "+human(total):"")+
    (sp?"  |  "+human(sp)+"/s":"")+(eta>1?"  |  "+Math.ceil(eta)+"s":"");
}
function ovDone(msg){ $("#ovBar").style.width="100%"; $("#ovPct").textContent="100%";
  $("#ovInfo").textContent=msg||t("done"); $("#ovBtn").textContent=t("close"); }
function ovError(msg){ $("#ovErr").textContent=msg; $("#ovBtn").textContent=t("close"); }
$("#ovBtn").onclick=()=>{ if(ovCancel){ ovCancel(); } else ovClose(); };

async function download(full,name,size){
  ovOpen(t("downloading"),name);
  const ctrl=new AbortController(); ovCancel=()=>{ ctrl.abort(); fetch("/api/abort",{method:"POST"}).catch(()=>{}); ovClose(); };
  try{
    const resp=await fetch("/api/dl?path="+encodeURIComponent(full),{signal:ctrl.signal});
    if(!resp.ok) throw new Error("HTTP "+resp.status);
    const total=size||+resp.headers.get("Content-Length")||0;
    const reader=resp.body.getReader(); const chunks=[]; let got=0; const t0=performance.now();
    for(;;){ const{done,value}=await reader.read(); if(done)break;
      chunks.push(value); got+=value.length; ovProg(got,total,(performance.now()-t0)/1000); }
    if(total && got<total){ ovCancel=null; ovError(t("incomplete",human(got),human(total))); return; }
    const url=URL.createObjectURL(new Blob(chunks));
    const a=document.createElement("a"); a.href=url; a.download=name; document.body.appendChild(a); a.click(); a.remove();
    URL.revokeObjectURL(url); ovCancel=null; ovDone(t("saved"));
  }catch(err){ if(err.name==="AbortError")return; ovCancel=null; ovError(t("fail",err.message)); }
}

function uploadOne(file){ return new Promise((res,rej)=>{
  const xhr=new XMLHttpRequest(); ovCancel=()=>{ xhr.abort(); fetch("/api/abort",{method:"POST"}).catch(()=>{}); ovClose(); };
  const t0=performance.now();
  xhr.open("POST","/api/up?path="+encodeURIComponent(join(cwd,file.name)));
  xhr.upload.onprogress=e=>{ if(e.lengthComputable) ovProg(e.loaded,e.total,(performance.now()-t0)/1000); };
  xhr.onload=()=>{ let ok=false,st=0; try{const j=JSON.parse(xhr.responseText); ok=j.ok; st=j.status;}catch{ ok=xhr.status<300; }
    ok?res():rej(new Error("status "+st)); };
  xhr.onerror=()=>rej(new Error("net")); xhr.onabort=()=>rej(new Error("__abort"));
  // multipart/form-data: the ESP WebServer drives server.upload() only for forms;
  // a raw body takes its raw path (null _currentUpload -> crash on ESP32).
  const fd=new FormData(); fd.append("f", file, file.name);
  xhr.send(fd);
}); }

async function upload(files){
  for(const f of files){
    ovOpen(t("uploading"),f.name);
    try{ await uploadOne(f); }
    catch(err){ if(err.message==="__abort")return; ovCancel=null; ovError(t("upFail",err.message)); return; }
  }
  ovCancel=null; ovDone(t("sent")); reload();
}

$("#file").onchange=e=>{ if(e.target.files.length) upload(e.target.files); e.target.value=""; };
const drop=$("#drop");
["dragenter","dragover"].forEach(ev=>drop.addEventListener(ev,e=>{e.preventDefault();drop.classList.add("over");}));
["dragleave","drop"].forEach(ev=>drop.addEventListener(ev,e=>{e.preventDefault();drop.classList.remove("over");}));
drop.addEventListener("drop",e=>{ if(e.dataTransfer.files.length) upload(e.dataTransfer.files); });

/* ---- firmware update (OTA): open the release ZIP in the browser, decompress
        with the native DecompressionStream, write the chosen files to /sd2snes/ ---- */
const OTA_DIR = "/sd2snes/";
const OTA_SETS = {                       // device_name -> files preselected for it
  "sd2snes mk.ii":   ["firmware.img", "menu.bin"],
  "sd2snes mk.iii":  ["firmware.im3", "m3nu.bin"],
  "fxpak pro stm32": ["m3nu.bin", "firmware.stm"]
};
const baseName = p => { const i = p.lastIndexOf("/"); return i < 0 ? p : p.slice(i + 1); };
const reqSet = () => OTA_SETS[(devName || "").trim().toLowerCase()] || [];
const isFirmware = n => /^firmware\.[^.]+$/i.test(n);

let otaEntries = null, otaBuf = null, otaDV = null;

function findEOCD(dv, len){                                   // End Of Central Directory
  const min = Math.max(0, len - 22 - 0xFFFF);
  for(let i = len - 22; i >= min; i--) if(dv.getUint32(i, true) === 0x06054b50) return i;
  return -1;
}
function parseZip(buf){
  const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  const eocd = findEOCD(dv, buf.length);
  if(eocd < 0) throw new Error("EOCD");
  const count = dv.getUint16(eocd + 10, true);
  let p = dv.getUint32(eocd + 16, true);                     // central directory offset
  const dec = new TextDecoder();
  const out = [];
  for(let n = 0; n < count && p + 46 <= buf.length; n++){
    if(dv.getUint32(p, true) !== 0x02014b50) break;          // central file header sig
    const method = dv.getUint16(p + 10, true);
    const csize  = dv.getUint32(p + 20, true);
    const usize  = dv.getUint32(p + 24, true);
    const nlen   = dv.getUint16(p + 28, true);
    const elen   = dv.getUint16(p + 30, true);
    const clen   = dv.getUint16(p + 32, true);
    const lho    = dv.getUint32(p + 42, true);               // local header offset
    const name   = dec.decode(buf.subarray(p + 46, p + 46 + nlen));
    if(!name.endsWith("/")) out.push({ name, base: baseName(name), method, csize, usize, lho });
    p += 46 + nlen + elen + clen;
  }
  return { entries: out, dv };
}
async function inflateRaw(bytes){
  if(!("DecompressionStream" in window)) throw new Error("no inflate");
  const s = new Blob([bytes]).stream().pipeThrough(new DecompressionStream("deflate-raw"));
  return new Uint8Array(await new Response(s).arrayBuffer());
}
async function entryBytes(e){
  if(otaDV.getUint32(e.lho, true) !== 0x04034b50) throw new Error("LFH");   // local file header
  const nlen = otaDV.getUint16(e.lho + 26, true);
  const elen = otaDV.getUint16(e.lho + 28, true);
  const start = e.lho + 30 + nlen + elen;
  const comp = otaBuf.subarray(start, start + e.csize);
  if(e.method === 0) return comp.slice();        // stored
  if(e.method === 8) return inflateRaw(comp);     // deflate
  throw new Error("method " + e.method);
}

function otaClose(){ $("#otaov").classList.remove("show"); otaEntries = otaBuf = otaDV = null; }
/* reset the OTA modal chrome - shared by the local .zip picker and the GitHub install */
function otaReset(name){
  $("#b_otatitle").textContent = t("otaTitle");
  $("#otaname").textContent    = name;
  $("#b_otago").textContent    = t("otaWrite");
  $("#b_otacancel").textContent= t("cancel"); $("#b_otacancel").style.display = "";
  $("#b_otago").disabled = false; $("#b_otago").style.display = "";
  $("#otabarwrap").style.display = "none"; $("#otastat").style.display = "none";
  $("#otamsg").className = "msg"; $("#otamsg").textContent = "";
  $("#otalist").innerHTML = "";
  $("#otaov").classList.add("show");
}
/* parse the ZIP already in otaBuf and render the file-selection list */
function otaRenderEntries(){
  try{ const z = parseZip(otaBuf); otaEntries = z.entries; otaDV = z.dv; }
  catch(err){ $("#otamsg").className="msg err"; $("#otamsg").textContent=t("otaZipErr", err.message); $("#b_otago").style.display="none"; return; }
  if(!otaEntries.length){ $("#otamsg").className="msg err"; $("#otamsg").textContent=t("otaEmpty"); $("#b_otago").style.display="none"; return; }
  $("#b_otago").style.display=""; $("#otamsg").className="msg"; $("#otamsg").textContent = t("otaPick");
  const want = reqSet();
  otaEntries.sort((a,b)=>a.name.localeCompare(b.name,undefined,{numeric:true}));
  const list = $("#otalist"); list.innerHTML="";
  otaEntries.forEach((e,i)=>{
    e.dest = OTA_DIR + e.base;
    const pre = want.includes(e.base.toLowerCase());
    const row=document.createElement("label"); row.className="otarow"+(pre?" req":"");
    const cb=document.createElement("input"); cb.type="checkbox"; cb.checked=pre; cb.dataset.i=i;
    const nm=document.createElement("span"); nm.className="of"; nm.textContent=e.name;
    const sz=document.createElement("span"); sz.className="os"; sz.textContent=human(e.usize);
    row.append(cb,nm,sz); list.appendChild(row);
  });
}
async function otaPicked(file){
  otaReset(file.name);
  try{ otaBuf = new Uint8Array(await file.arrayBuffer()); }
  catch(err){ $("#otamsg").className="msg err"; $("#otamsg").textContent=t("otaZipErr", err.message); $("#b_otago").style.display="none"; return; }
  otaRenderEntries();
}
function otaPut(dest, bytes){ return new Promise((res,rej)=>{
  const xhr=new XMLHttpRequest(); const t0=performance.now();
  xhr.open("POST","/api/up?path="+encodeURIComponent(dest));
  xhr.upload.onprogress=ev=>{ if(ev.lengthComputable){
    const pct=Math.min(100,ev.loaded/ev.total*100);
    $("#otaBar").style.width=pct+"%"; $("#otaPct").textContent=pct.toFixed(0)+"%";
    const secs=(performance.now()-t0)/1000, sp=secs>0?ev.loaded/secs:0;
    $("#otaInfo").textContent=human(ev.loaded)+" / "+human(ev.total)+(sp?"  |  "+human(sp)+"/s":"");
  }};
  xhr.onload=()=>{ let ok=false,st=0; try{const j=JSON.parse(xhr.responseText); ok=j.ok; st=j.status;}catch{ ok=xhr.status<300; }
    ok?res():rej(new Error("status "+st)); };
  xhr.onerror=()=>rej(new Error("net"));
  // multipart/form-data (see uploadOne): raw bodies crash the ESP WebServer.
  const fd=new FormData(); fd.append("f", new Blob([bytes]), baseName(dest));
  xhr.send(fd);
}); }
async function otaWrite(){
  const boxes = [...$("#otalist").querySelectorAll("input:checked")];
  if(!boxes.length){ $("#otamsg").className="msg err"; $("#otamsg").textContent=t("otaNone"); return; }
  const sel = boxes.map(b=>otaEntries[+b.dataset.i]);
  $("#b_otago").disabled = true; $("#b_otacancel").style.display="none";
  $("#otabarwrap").style.display=""; $("#otastat").style.display="";
  $("#otamsg").className="msg";
  let firmware = false;
  for(let k=0; k<sel.length; k++){
    const e = sel[k];
    $("#otamsg").textContent = t("otaWriting", k+1, sel.length, e.base);
    $("#otaBar").style.width="0"; $("#otaPct").textContent="0%"; $("#otaInfo").textContent="";
    let bytes;
    try{ bytes = await entryBytes(e); await otaPut(e.dest, bytes); }
    catch(err){ $("#otamsg").className="msg err"; $("#otamsg").textContent=t("otaFail", e.base, err.message);
      $("#b_otago").disabled=false; $("#b_otacancel").style.display=""; return; }
    if(isFirmware(e.base)) firmware = true;
  }
  $("#otaBar").style.width="100%"; $("#otaPct").textContent="100%"; $("#otaInfo").textContent="";
  $("#b_otago").style.display="none"; $("#b_otacancel").style.display=""; $("#b_otacancel").textContent=t("close");
  $("#otamsg").className = firmware ? "msg warn" : "msg ok";
  $("#otamsg").textContent = firmware ? t("otaDonePower") : t("otaDoneMenu");
  reload();
}
$("#zip").onchange=e=>{ if(e.target.files.length) otaPicked(e.target.files[0]); e.target.value=""; };

/* ---- firmware releases from GitHub (list + notes) with 1-click install ----
   The release LIST comes from api.github.com, which sends "Access-Control-Allow-Origin: *",
   so it reads fine cross-origin even from the device's http:// page (an http page fetching
   https is allowed - mixed content only blocks the reverse). The ZIP asset itself CAN'T be
   fetched from GitHub cross-origin (its release-assets host omits CORS), so the download goes
   through the fork's own proxy (sd2snes.ludufre.com/api/firmware), which must send CORS for
   this origin. The bytes then feed the existing OTA select+write flow. Needs internet (STA). */
const GH_API   = "https://api.github.com/repos/ludufre/sd2snes/releases?per_page=20";
const FW_PROXY = "https://sd2snes.ludufre.com/api/firmware";   // ?id=<assetId>
let fwReleases = null, fwSel = null;

/* minimal injection-safe markdown -> HTML for the changelog (escape first, then transform) */
function esc(s){ return s.replace(/[&<>]/g, c=>({"&":"&amp;","<":"&lt;",">":"&gt;"}[c])); }
function mdInline(s){
  return esc(s)
    .replace(/`([^`]+)`/g, "<code>$1</code>")
    .replace(/\*\*([^*]+)\*\*/g, "<b>$1</b>")
    .replace(/(^|[^*])\*([^*]+)\*/g, "$1<i>$2</i>")
    .replace(/!\[[^\]]*\]\([^)]*\)/g, "")                       // drop images (hotlinks 302 -> fail)
    .replace(/\[([^\]]+)\]\((https?:\/\/[^)\s]+)\)/g, '<a href="$2" target="_blank" rel="noopener">$1</a>');
}
function mdToHtml(src){
  const lines = (src||"").replace(/\r/g,"").split("\n");
  let html="", inList=false, inCode=false, quote=null;
  const closeList=()=>{ if(inList){ html+="</ul>"; inList=false; } };
  const flushQuote=()=>{                                    // render a `>` blockquote / GitHub alert
    if(!quote) return;
    let type=null, body=quote;
    const m0 = quote.length && quote[0].match(/^\[!(NOTE|TIP|IMPORTANT|WARNING|CAUTION)\]\s*$/i);
    if(m0){ type=m0[1].toLowerCase(); body=quote.slice(1); }
    const paras=[]; let cur=[];
    for(const q of body){ if(!q.trim()){ if(cur.length){ paras.push(cur.join(" ")); cur=[]; } } else cur.push(q); }
    if(cur.length) paras.push(cur.join(" "));
    const inner = paras.map(p=>"<p>"+mdInline(p)+"</p>").join("");
    html += type ? '<div class="gh-alert '+type+'"><div class="ga-t">'+type.toUpperCase()+"</div>"+inner+"</div>"
                 : "<blockquote>"+inner+"</blockquote>";
    quote=null;
  };
  for(let raw of lines){
    if(/^```/.test(raw)){ flushQuote(); if(inCode){ html+="</code></pre>"; inCode=false; } else { closeList(); html+="<pre><code>"; inCode=true; } continue; }
    if(inCode){ html+=esc(raw)+"\n"; continue; }
    // drop raw HTML media/structural tags GitHub allows in bodies (they'd show as literal
    // text once escaped, and githubusercontent images 302 to signed URLs that fail cross-origin)
    raw = raw.replace(/<img\b[^>]*>/gi,"").replace(/<\/?(?:video|picture|source|details|summary|div|span|br|p)\b[^>]*\/?>/gi,"");
    const qm = raw.match(/^\s*>\s?(.*)$/);                   // blockquote line -> accumulate
    if(qm){ closeList(); (quote=quote||[]).push(qm[1]); continue; }
    flushQuote();
    const line=raw.trim();
    if(!line){ closeList(); continue; }
    let m;
    if((m=line.match(/^(#{1,3})\s+(.*)$/))){ closeList(); const n=m[1].length; html+="<h"+n+">"+mdInline(m[2])+"</h"+n+">"; continue; }
    if(/^(-{3,}|\*{3,}|_{3,})$/.test(line)){ closeList(); html+="<hr>"; continue; }
    if((m=line.match(/^[-*+]\s+(.*)$/))){ if(!inList){ html+="<ul>"; inList=true; } html+="<li>"+mdInline(m[1])+"</li>"; continue; }
    closeList(); html+="<p>"+mdInline(line)+"</p>";
  }
  closeList(); flushQuote(); if(inCode) html+="</code></pre>";
  return html || '<p class="dim">'+esc(t("fwNotesEmpty"))+"</p>";
}

function fwZipAssets(r){                     // pick the core "update" + "full" zips from a release
  let update, full;
  for(const a of (r.assets||[])){
    if(!/\.zip$/i.test(a.name)) continue;
    const asset={ id:a.id, name:a.name, size:a.size };
    if(/-full\.zip$/i.test(a.name)) full=asset; else update=asset;
  }
  return { update, full };
}
function fwClose(){ $("#fwov").classList.remove("show"); }
async function fwOpen(){
  $("#b_fwtitle").textContent=t("otaTitle");
  $("#fwbody").style.display="none"; $("#fwfoot").style.display="none";
  $("#fwmsg").style.display=""; $("#fwmsg").className="fwmsg"; $("#fwmsg").textContent=t("fwLoading");
  $("#fwov").classList.add("show");
  if(fwReleases){ fwShow(); return; }
  try{
    const r=await fetch(GH_API,{headers:{Accept:"application/vnd.github+json"}});
    if(!r.ok) throw new Error("HTTP "+r.status);
    fwReleases=(await r.json()).map(x=>({ tag:x.tag_name, name:x.name||x.tag_name,
      date:(x.published_at||"").slice(0,10), pre:!!x.prerelease, body:x.body||"", ...fwZipAssets(x) }));
    fwShow();
  }catch(err){ $("#fwmsg").className="fwmsg err"; $("#fwmsg").textContent=t("fwLoadErr", err.message); }
}
function fwShow(){
  if(!fwReleases.length){ $("#fwmsg").className="fwmsg"; $("#fwmsg").textContent=t("fwNone"); return; }
  $("#fwmsg").style.display="none"; $("#fwbody").style.display=""; $("#fwfoot").style.display="";
  const list=$("#fwlist"); list.innerHTML="";
  fwReleases.forEach(r=>{
    const b=document.createElement("button"); b.className="ritem"; b.type="button";
    b.innerHTML='<span class="rname"></span><span class="rmeta"></span>';
    b.querySelector(".rname").textContent=r.name;
    if(r.pre){ const p=document.createElement("i"); p.className="pre"; p.textContent="pre"; b.querySelector(".rname").appendChild(p); }
    b.querySelector(".rmeta").textContent=r.tag+" · "+r.date;
    b.onclick=()=>fwSelect(r,b); r._btn=b; list.appendChild(b);
  });
  fwSelect(fwReleases[0], fwReleases[0]._btn);
}
function fwSelect(r,btn){
  fwSel=r;
  $("#fwlist").querySelectorAll(".ritem").forEach(x=>x.classList.remove("on"));
  if(btn) btn.classList.add("on");
  $("#fwnotes").innerHTML=mdToHtml(r.body);
  const bt=$("#fwbtns"); bt.innerHTML="";
  const mk=(asset,label,cls)=>{ if(!asset) return;
    const b=document.createElement("button"); b.className=cls; b.type="button";
    b.innerHTML=svg(IC.download)+"<span></span>"; b.lastChild.textContent=label+" · "+human(asset.size);
    b.onclick=()=>fwInstall(asset); bt.appendChild(b); };
  mk(r.update, t("fwUpdate"), "");
  mk(r.full,   t("fwFull"),   "primary");
}
async function fwInstall(asset){
  fwClose();
  otaReset(asset.name);
  $("#otamsg").textContent=t("fwDownloading");
  $("#otabarwrap").style.display=""; $("#otastat").style.display="";
  $("#b_otago").style.display="none";
  $("#otaBar").style.width="0"; $("#otaPct").textContent="0%"; $("#otaInfo").textContent="";
  try{
    const resp=await fetch(FW_PROXY+"?id="+asset.id);
    if(!resp.ok) throw new Error("HTTP "+resp.status);
    const total=asset.size||+resp.headers.get("Content-Length")||0;
    const reader=resp.body.getReader(); const chunks=[]; let got=0; const t0=performance.now();
    for(;;){ const{done,value}=await reader.read(); if(done)break;
      chunks.push(value); got+=value.length;
      const pct=total?Math.min(100,got/total*100):0, secs=(performance.now()-t0)/1000, sp=secs>0?got/secs:0;
      $("#otaBar").style.width=pct+"%"; $("#otaPct").textContent=(total?pct.toFixed(0):"-")+"%";
      $("#otaInfo").textContent=human(got)+(total?" / "+human(total):"")+(sp?"  |  "+human(sp)+"/s":"");
    }
    otaBuf=new Uint8Array(got); let o=0; for(const c of chunks){ otaBuf.set(c,o); o+=c.length; }
  }catch(err){ $("#otabarwrap").style.display="none"; $("#otastat").style.display="none";
    $("#otamsg").className="msg err"; $("#otamsg").textContent=t("fwDlErr", err.message); return; }
  $("#otabarwrap").style.display="none"; $("#otastat").style.display="none";
  otaRenderEntries();
}
function fwLocal(){ fwClose(); $("#zip").click(); }

/* firmware update chooser: pick online (GitHub) vs local .zip. Online needs the
   ESP joined to a WiFi network (STA mode) so the browser can reach GitHub; in
   AP-only mode there's no internet, so we ask the user to connect to WiFi first. */
function fwPickClose(){ $("#fwpick").classList.remove("show"); }
function fwPick(){
  $("#fwpickTitle").textContent = t("otaTitle");
  $("#fwpickSub").textContent   = t("fwHow");
  $("#fwpickMsg").className = "msg"; $("#fwpickMsg").textContent = "";
  $("#fwpickClose").textContent = t("close");
  $("#fwpick").classList.add("show");
}
async function fwPickOnline(){
  $("#fwpickMsg").className = "msg"; $("#fwpickMsg").textContent = t("fwChecking");
  let connected = false;
  try{ const s = await(await fetch("/api/wifi/status")).json(); connected = !!(s.connected && s.ip); }catch{}
  if(!connected){                       // AP-only / not joined -> no internet for GitHub
    $("#fwpickMsg").className = "msg warn"; $("#fwpickMsg").textContent = "";
    const span = document.createElement("span"); span.textContent = t("fwNeedWifi") + " ";
    const a = document.createElement("a"); a.textContent = t("fwOpenWifi");
    a.style.cssText = "cursor:pointer;color:var(--accent);text-decoration:underline";
    a.onclick = () => { fwPickClose(); wifiOpen(); };
    $("#fwpickMsg").append(span, a);
    return;
  }
  fwPickClose(); fwOpen();
}
function fwPickLocal(){ fwPickClose(); $("#zip").click(); }

/* ---- WiFi ---- */
const wov=$("#wov");
function wclose(){ wov.classList.remove("show"); }
async function wifiOpen(){ wov.classList.add("show"); await wifiStatus(); wifiScan(); }
async function wifiStatus(){
  try{ const s=await(await fetch("/api/wifi/status")).json();
    $("#wstat").textContent = s.connected
      ? t("connected", s.ssid, s.ip+(s.rssi?", "+s.rssi+" dBm":""))
      : t("notConn");
    $("#wforget").style.display = s.connected? "" : "none";
  }catch{ $("#wstat").textContent=t("stUnavail"); }
}
function bars(rssi){ const p=Math.max(0,Math.min(100,2*(rssi+100)));
  return p>=75?"▂▄▆█":p>=50?"▂▄▆":p>=25?"▂▄":"▂"; }
async function wifiScan(){
  $("#wscan").disabled=true; $("#wlist").innerHTML='<div class="wmsg">'+t("scanning")+'</div>';
  try{
    const aps=await(await fetch("/api/wifi/scan")).json();
    const el=$("#wlist"); el.innerHTML="";
    if(!aps.length){ el.innerHTML='<div class="wmsg">'+t("noNets")+'</div>'; }
    aps.forEach(a=>{
      const row=document.createElement("div"); row.className="wrow";
      row.innerHTML='<span class="wsig"></span><span class="wssid"></span><span class="wlock">'+(a.enc?svg(IC.lock):"")+'</span>';
      row.querySelector(".wsig").textContent=bars(a.rssi);
      row.querySelector(".wssid").textContent=a.ssid;
      row.onclick=()=>wifiPick(a,row);
      el.appendChild(row);
    });
  }catch{ $("#wlist").innerHTML='<div class="wmsg">'+t("scanFail")+'</div>'; }
  $("#wscan").disabled=false;
}
function wifiPick(a,row){
  document.querySelectorAll(".wform").forEach(f=>f.remove());
  if(!a.enc){ wifiDo(a.ssid,""); return; }
  const f=document.createElement("div"); f.className="wform";
  const inp=document.createElement("input"); inp.type="password"; inp.placeholder=t("password");
  const btn=document.createElement("button"); btn.className="primary"; btn.textContent=t("connect");
  f.append(inp,btn);
  btn.onclick=()=>wifiDo(a.ssid,inp.value);
  inp.onkeydown=e=>{ if(e.key==="Enter") wifiDo(a.ssid,inp.value); };
  row.after(f); inp.focus();
}
async function wifiDo(ssid,pass){
  document.querySelectorAll(".wform").forEach(f=>f.remove());
  $("#wstat").textContent=t("connecting",ssid);
  try{ await fetch("/api/wifi/connect?ssid="+encodeURIComponent(ssid)+"&pass="+encodeURIComponent(pass),{method:"POST"}); }catch{}
  for(let i=0;i<12;i++){ await new Promise(r=>setTimeout(r,1000));
    try{ const s=await(await fetch("/api/wifi/status")).json();
      if(s.connected && s.ip){
        $("#wforget").style.display="";
        $("#wstat").textContent=t("opening",s.ssid,s.ip,s.ip);
        setTimeout(()=>{ location.href="http://"+s.ip+"/"; }, 1500);
        return;
      }
    }catch{}
  }
  $("#wstat").textContent=t("connFail",ssid);
}
async function wifiForget(){ if(!confirm(t("cfmForget")))return; try{ await fetch("/api/wifi/forget",{method:"POST"}); }catch{} wifiStatus(); }

/* block zoom (pinch/double-tap), including iOS which ignores user-scalable=no */
["gesturestart","gesturechange","gestureend"].forEach(ev=>document.addEventListener(ev,e=>e.preventDefault()));
let _lt=0; document.addEventListener("touchend",e=>{ const n=Date.now(); if(n-_lt<=300) e.preventDefault(); _lt=n; }, {passive:false});

/* device pill in the topbar: green dot + name + proto (matches Manager .conn) */
function setDev(name, ver){
  const d=$("#dev"); d.className="dev conn"; d.innerHTML="";
  const dot=document.createElement("span"); dot.className="dot";
  const nm=document.createElement("span"); nm.className="nm"; nm.textContent=name;
  const sep=document.createElement("span"); sep.className="sep"; sep.textContent="·";
  const pv=document.createElement("span"); pv.textContent="proto v"+ver;
  d.append(dot,nm,sep,pv);
}
function setDevOff(msg){ const d=$("#dev"); d.className="dev conn off"; d.textContent=msg; }

/* the cover preview + firmware modals close on backdrop click or Escape (no side effects) */
$("#cov").addEventListener("click", e=>{ if(e.target===$("#cov")) covClose(); });
$("#fwov").addEventListener("click", e=>{ if(e.target===$("#fwov")) fwClose(); });
$("#fwpick").addEventListener("click", e=>{ if(e.target===$("#fwpick")) fwPickClose(); });
document.addEventListener("keydown", e=>{ if(e.key!=="Escape") return;
  if($("#cov").classList.contains("show")) covClose();
  else if($("#fwov").classList.contains("show")) fwClose();
  else if($("#fwpick").classList.contains("show")) fwPickClose(); });

/* ---- section router (Shelf / Files / Saves / Settings / Device) ---- */
const TABS = ["shelf","files","saves","settings","device"];
const tabInited = {};
function wipTab(sel){ document.querySelector(sel+" .wrap").innerHTML='<div class="wip">'+esc(t("wip"))+"</div>"; }
function tabInit(name){
  if(name==="shelf") return shelfBuild();
  if(name==="files"){ if(!tabInited.__files){ tabInited.__files=1; go("/"); } return; }
  if(name==="saves")    return savesBuild();
  if(name==="settings") return settingsBuild();
  if(name==="device")   return deviceBuild();
}
function showTab(name){
  if(!TABS.includes(name)) name="shelf";
  TABS.forEach(tk=>{ $("#tab-"+tk).classList.toggle("on", tk===name);
    const b=document.querySelector('.tabs [data-tab="'+tk+'"]'); if(b) b.classList.toggle("on", tk===name); });
  if(!tabInited[name]){ tabInited[name]=1; try{ tabInit(name); }catch{} }
  if(("#"+name)!==location.hash) history.replaceState(null,"","#"+name);
}
document.querySelectorAll('.tabs [data-tab]').forEach(b=>b.onclick=()=>showTab(b.dataset.tab));
window.addEventListener("hashchange",()=>showTab(location.hash.slice(1)||"shelf"));

/* ---- Shelf: cover-art home from lastgame.cfg (recents) + favorites.cfg ---- */
const LAST_FILE="/sd2snes/lastgame.cfg", FAVORITES_FILE="/sd2snes/favorites.cfg";
const covPath = rom => { const i=rom.lastIndexOf("."); return (i>0?rom.slice(0,i):rom)+".cov"; };
const romStem = rom => { const b=baseName(rom); const i=b.lastIndexOf("."); return i>0?b.slice(0,i):b; };
async function fetchList(path){                 // NUL-terminated, /-rooted, patch-aware <rom>\t<patch>
  try{
    const r=await fetch("/api/dl?path="+encodeURIComponent(path));
    if(!r.ok) return [];
    const txt=new TextDecoder().decode(new Uint8Array(await r.arrayBuffer()));
    return txt.split("\0").map(s=>s.trim()).filter(s=>s.startsWith("/"))
      .map(s=>{ const p=s.split("\t"); return { rom:p[0], patch:p[1]||null }; });
  }catch{ return []; }
}
function shelfTile(e, hero){
  const tile=document.createElement("button"); tile.className="stile"+(hero?" hero":""); tile.type="button";
  const art=document.createElement("div"); art.className="art"; art.innerHTML=svg(IC.image);
  art.dataset.cov=covPath(e.rom);
  const cap=document.createElement("div"); cap.className="cap"; cap.textContent=romStem(e.rom);
  tile.append(art,cap); tile.onclick=()=>covPreview(covPath(e.rom), romStem(e.rom), e.rom, e.patch);
  return { tile, art };
}
async function shelfBuild(){
  const host=$("#shelf"); host.innerHTML='<div class="empty"><span class="spin"></span></div>';
  const [rec,fav]=await Promise.all([fetchList(LAST_FILE),fetchList(FAVORITES_FILE)]);
  host.innerHTML="";
  const all=[...rec,...fav];
  if(!all.length){ host.innerHTML='<div class="shelf-empty">'+esc(t("shelfEmpty"))+"</div>"; return; }
  const arts=[];
  const top=document.createElement("div"); top.className="shelf-top";
  const rnd=document.createElement("button"); rnd.className="btn sm"; rnd.innerHTML=svg(IC.play)+"<span></span>";
  rnd.lastChild.textContent=t("shelfRandom");
  rnd.onclick=()=>{ const e=all[Math.floor(Math.random()*all.length)]; covPreview(covPath(e.rom),romStem(e.rom),e.rom,e.patch); };
  top.appendChild(rnd); host.appendChild(top);
  const section=(titleKey,list)=>{
    if(!list.length) return;
    const sec=document.createElement("div"); sec.className="ssec";
    const h=document.createElement("div"); h.className="ssec-h"; h.textContent=t(titleKey); sec.appendChild(h);
    const grid=document.createElement("div"); grid.className="sgrid";
    list.forEach(e=>{ const {tile,art}=shelfTile(e); grid.appendChild(tile); arts.push(art); });
    sec.appendChild(grid); host.appendChild(sec);
  };
  if(rec.length){
    const sec=document.createElement("div"); sec.className="ssec";
    const h=document.createElement("div"); h.className="ssec-h"; h.textContent=t("shelfContinue"); sec.appendChild(h);
    const {tile,art}=shelfTile(rec[0], true); sec.appendChild(tile); arts.push(art); host.appendChild(sec);
  }
  section("shelfFavorites", fav);
  section("shelfRecents", rec.slice(1));
  // covers are few (<=30) and small: load them all serially (robust to tab switches)
  arts.forEach(a=>covEnqueue(()=>covThumb(a)));
}

/* ---- Saves: back up / restore .srm/.state as a store-only browser zip ---- */
const CRC_T=(()=>{const t=new Uint32Array(256);for(let n=0;n<256;n++){let c=n;for(let k=0;k<8;k++)c=c&1?0xEDB88320^(c>>>1):c>>>1;t[n]=c>>>0;}return t;})();
function crc32(b){ let c=0xFFFFFFFF; for(let i=0;i<b.length;i++) c=CRC_T[(c^b[i])&0xff]^(c>>>8); return (c^0xFFFFFFFF)>>>0; }
function makeZip(files){                       // store (method 0), no compression - saves are small
  const enc=new TextEncoder();
  const u16=v=>[v&0xff,(v>>8)&0xff], u32=v=>[v&0xff,(v>>8)&0xff,(v>>16)&0xff,(v>>>24)&0xff];
  const parts=[], cent=[]; let off=0;
  for(const f of files){
    const nm=enc.encode(f.name), d=f.data, crc=crc32(d);
    const lfh=new Uint8Array([...u32(0x04034b50),...u16(20),...u16(0),...u16(0),...u16(0),...u16(0),...u32(crc),...u32(d.length),...u32(d.length),...u16(nm.length),...u16(0)]);
    parts.push(lfh,nm,d);
    cent.push(new Uint8Array([...u32(0x02014b50),...u16(20),...u16(20),...u16(0),...u16(0),...u16(0),...u16(0),...u32(crc),...u32(d.length),...u32(d.length),...u16(nm.length),...u16(0),...u16(0),...u16(0),...u16(0),...u32(0),...u32(off)]),nm);
    off+=lfh.length+nm.length+d.length;
  }
  let cdSize=0; cent.forEach(p=>cdSize+=p.length);
  const eocd=new Uint8Array([...u32(0x06054b50),...u16(0),...u16(0),...u16(files.length),...u16(files.length),...u32(cdSize),...u32(off),...u16(0)]);
  const all=[...parts,...cent,eocd]; let total=0; all.forEach(p=>total+=p.length);
  const out=new Uint8Array(total); let o=0; for(const p of all){ out.set(p,o); o+=p.length; } return out;
}
async function zipExtract(buf){                // reuses parseZip + inflateRaw (from OTA)
  const {entries,dv}=parseZip(buf); const out=[];
  for(const e of entries){
    if(dv.getUint32(e.lho,true)!==0x04034b50) continue;
    const nlen=dv.getUint16(e.lho+26,true), elen=dv.getUint16(e.lho+28,true);
    const start=e.lho+30+nlen+elen, comp=buf.subarray(start,start+e.csize);
    let data; if(e.method===0) data=comp.slice(); else if(e.method===8) data=await inflateRaw(comp); else continue;
    out.push({name:e.name, data});
  }
  return out;
}
const isSave=n=>/\.(srm|state)$/i.test(n);
/* the firmware writes .srm to SAVE_BASEDIR and .state to SS_BASEDIR (memory.h / savestate.h) -
   both FLAT folders, so just list those two instead of walking the whole SD tree (which was one
   slow UART round-trip PER folder). */
const SAVE_DIRS=["/sd2snes/saves","/sd2snes/states"];
async function scanSaves(out,prog){
  for(const dir of SAVE_DIRS){
    let j; try{ const r=await fetch("/api/ls?path="+encodeURIComponent(dir)); if(!r.ok) continue; j=await r.json(); }catch{ continue; }
    for(const e of (j.entries||[])){ if(e.dir||hidden(e.name)) continue;
      if(isSave(e.name)){ out.push({path:join(dir,e.name),size:e.size}); prog&&prog(out.length); } }
  }
}
let savesFound=[];
function savesBuild(){
  const w=document.querySelector("#tab-saves .wrap"); w.innerHTML=""; savesFound=[];
  const head=document.createElement("div"); head.className="sect-head";
  const h=document.createElement("h2"); h.textContent=t("savesTitle"); head.appendChild(h); w.appendChild(head);
  const intro=document.createElement("p"); intro.className="sect-intro"; intro.textContent=t("savesIntro"); w.appendChild(intro);
  const bar=document.createElement("div"); bar.className="sect-bar";
  const scan=document.createElement("button"); scan.className="btn primary"; scan.innerHTML=svg(IC.refresh)+"<span></span>"; scan.lastChild.textContent=t("savesScan");
  const restore=document.createElement("label"); restore.className="btn";
  restore.innerHTML=svg(IC.upload)+"<span></span>"; restore.querySelector("span").textContent=t("savesRestore");
  const rin=document.createElement("input"); rin.type="file"; rin.accept=".zip"; rin.hidden=true; restore.appendChild(rin);
  bar.append(scan,restore); w.appendChild(bar);
  const st=document.createElement("div"); st.id="savesStatus"; st.className="sect-status"; w.appendChild(st);
  const list=document.createElement("div"); list.id="savesList"; list.className="saveslist"; w.appendChild(list);
  const runScan=async()=>{ scan.disabled=true; savesFound=[]; st.textContent=t("savesScanning",0);
    await scanSaves(savesFound,n=>{ st.textContent=t("savesScanning",n); });
    scan.disabled=false; savesRender(); };
  scan.onclick=runScan;
  rin.onchange=e=>{ if(e.target.files[0]) savesRestore(e.target.files[0]); e.target.value=""; };
  runScan();   // auto-scan on open - fast now (2 flat listings, not a whole-SD walk)
}
function savesRender(){
  const list=$("#savesList"), st=$("#savesStatus"); list.innerHTML="";
  if(!savesFound.length){ st.textContent=t("savesNone"); return; }
  st.textContent="";
  const hdr=document.createElement("div"); hdr.className="saverow hdr";
  const all=document.createElement("input"); all.type="checkbox"; all.checked=true;
  const alllbl=document.createElement("span"); alllbl.className="sf"; alllbl.textContent=t("savesAll");
  const bk=document.createElement("button"); bk.className="btn sm primary"; bk.innerHTML=svg(IC.download)+"<span></span>"; bk.lastChild.textContent=t("savesBackup");
  hdr.append(all,alllbl,bk); list.appendChild(hdr);
  savesFound.forEach((s,i)=>{
    const row=document.createElement("label"); row.className="saverow";
    const cb=document.createElement("input"); cb.type="checkbox"; cb.checked=true; cb.dataset.i=i;
    const nm=document.createElement("span"); nm.className="sf"; nm.textContent=s.path;
    const sz=document.createElement("span"); sz.className="ss"; sz.textContent=human(s.size);
    row.append(cb,nm,sz); list.appendChild(row);
  });
  all.onchange=()=>list.querySelectorAll(".saverow:not(.hdr) input").forEach(c=>c.checked=all.checked);
  bk.onclick=savesBackup;
}
async function savesBackup(){
  const st=$("#savesStatus");
  const sel=[...$("#savesList").querySelectorAll(".saverow:not(.hdr) input:checked")].map(c=>savesFound[+c.dataset.i]);
  if(!sel.length){ st.textContent=t("savesPickNone"); return; }
  const files=[];
  for(let i=0;i<sel.length;i++){ st.textContent=t("savesBackingUp",i+1,sel.length);
    try{ const r=await fetch("/api/dl?path="+encodeURIComponent(sel[i].path)); if(!r.ok) throw 0;
      files.push({name:sel[i].path.replace(/^\//,""), data:new Uint8Array(await r.arrayBuffer())}); }catch{}
  }
  const url=URL.createObjectURL(new Blob([makeZip(files)],{type:"application/zip"}));
  const a=document.createElement("a"); a.href=url; a.download="sd2snes-saves.zip"; document.body.appendChild(a); a.click(); a.remove();
  URL.revokeObjectURL(url); st.textContent=t("savesBackupDone")+" ("+files.length+")";
}
async function mkdirP(dir){ const parts=dir.split("/").filter(Boolean); let acc="";
  for(const p of parts){ acc+="/"+p; try{ await fetch("/api/mkdir?path="+encodeURIComponent(acc),{method:"POST"}); }catch{} } }
async function savesRestore(file){
  const st=$("#savesStatus");
  let items; try{ items=await zipExtract(new Uint8Array(await file.arrayBuffer())); }catch{ st.textContent=t("otaZipErr","zip"); return; }
  let ok=0;
  for(let i=0;i<items.length;i++){ st.textContent=t("savesRestoring",i+1,items.length);
    const dest="/"+items[i].name.replace(/^\//,""); const dir=dest.slice(0,dest.lastIndexOf("/"));
    if(dir) await mkdirP(dir);
    try{ await otaPut(dest, items[i].data); ok++; }catch{}
  }
  st.textContent=t("savesRestoreDone",ok);
}

/* ---- Settings: labeled config.yml editor (text round-trip preserves comments + unknown keys) ---- */
const CFG_FILE_PATH="/sd2snes/config.yml";
const CFG_LANG={k:'Language',t:'select',d:0,lbl:{"en": "Language", "pt": "Idioma", "es": "Idioma", "de": "Sprache"},opts:{en:["English", "Português BR", "Español", "Deutsch"],pt:["English", "Português BR", "Español", "Deutsch"],es:["English", "Português BR", "Español", "Deutsch"],de:["English", "Português BR", "Español", "Deutsch"]}};
const CFG_GROUPS=[
  {title:{"en": "BS-X Settings", "pt": "Opções BS-X", "es": "Opciones BS-X", "de": "BS-X Optionen"},fields:[
    {k:'BSXUseUsertime',t:'bool',lbl:{"en": "BS-X clock", "pt": "Relógio BS-X", "es": "Reloj BS-X", "de": "BS-X Uhr"},d:'false'},
  ]},
  {title:{"en": "Browser Settings", "pt": "Opções do Navegador", "es": "Opciones del Navegador", "de": "Browser-Optionen"},fields:[
    {k:'SortDirectories',t:'bool',lbl:{"en": "Sort directories", "pt": "Ordenar diretórios", "es": "Ordenar directorios", "de": "Ordner sortieren"},d:'true'},
    {k:'HideExtensions',t:'bool',lbl:{"en": "Hide file extensions", "pt": "Ocultar extensões", "es": "Ocultar extensiones", "de": "Endungen ausblenden"},d:'false'},
    {k:'EnableScreensaver',t:'bool',lbl:{"en": "Screensaver", "pt": "Protetor de tela", "es": "Protector de pantalla", "de": "Bildschirmschoner"},d:'true'},
    {k:'LEDBrightness',t:'range',lbl:{"en": "LED brightness", "pt": "Brilho dos LEDs", "es": "Brillo de los LEDs", "de": "LED-Helligkeit"},min:0,max:15,d:'15'},
    {k:'ShowCovers',t:'select',lbl:{"en": "Show covers", "pt": "Mostrar capas", "es": "Mostrar carátulas", "de": "Cover anzeigen"},opts:{"en": ["Off", "Large", "Small"], "pt": ["Deslig.", "Grande", "Pequeno"], "es": ["Apag.", "Grande", "Pequeño"], "de": ["Aus", "Groß", "Klein"]},d:'1'},
    {k:'ShowCoversInLists',t:'bool',lbl:{"en": "Favorites/Recents", "pt": "Favoritos/Recentes", "es": "Favoritos/Recientes", "de": "Favoriten/Letzte"},d:'true',sub:1},
    {k:'EnableMenuMusic',t:'bool',lbl:{"en": "Menu music", "pt": "Música do menu", "es": "Música del menú", "de": "Menue-Musik"},d:'true'},
    {k:'EnableMenuSFX',t:'bool',lbl:{"en": "Menu sounds", "pt": "Sons do menu", "es": "Sonidos del menú", "de": "Menue-Sounds"},d:'true'},
    {k:'SortFavorites',t:'bool',lbl:{"en": "Sort favorites", "pt": "Ordenar favoritos", "es": "Ordenar favoritos", "de": "Favoriten sortieren"},d:'true'},
    {k:'ShowGameInfo',t:'bool',lbl:{"en": "Game Info", "pt": "Ficha de Jogo", "es": "Info del Juego", "de": "Spielinfo"},d:'true'},
    {k:'GameInfoVideo',t:'bool',lbl:{"en": "Show video", "pt": "Exibir vídeo", "es": "Mostrar vídeo", "de": "Video zeigen"},d:'true',sub:1},
    {k:'GameInfoMusic',t:'bool',lbl:{"en": "Play video music", "pt": "Tocar música do vídeo", "es": "Música del vídeo", "de": "Videomusik"},d:'true',sub:1},
  ]},
  {title:{"en": "Chip Options", "pt": "Opções de Chip", "es": "Opciones de Chip", "de": "Chip-Optionen"},fields:[
    {k:'Cx4Speed',t:'select',lbl:{"en": "Cx4 speed", "pt": "Velocidade Cx4", "es": "Velocidad Cx4", "de": "Cx4 Geschwindigkeit"},opts:{"en": ["Original", "Fast"], "pt": ["Original", "Rápido"], "es": ["Original", "Rápido"], "de": ["Original", "Schnell"]},d:'0'},
    {k:'GSUSpeed',t:'select',lbl:{"en": "SuperFX speed", "pt": "Velocidade SuperFX", "es": "Velocidad SuperFX", "de": "SuperFX Geschw."},opts:{"en": ["Original", "Fast"], "pt": ["Original", "Rápido"], "es": ["Original", "Rápido"], "de": ["Original", "Schnell"]},d:'0'},
    {k:'MSUVolumeBoost',t:'select',lbl:{"en": "MSU-1 volume boost", "pt": "Boost volume MSU-1", "es": "Aumento volumen MSU-1", "de": "MSU-1 Lautstaerke"},opts:{"en": ["None", "+3.5 dB", "+6 dB", "+9.5 dB", "+12 dB"], "pt": ["Nenhum", "+3.5 dB", "+6 dB", "+9.5 dB", "+12 dB"], "es": ["Ninguno", "+3.5 dB", "+6 dB", "+9.5 dB", "+12 dB"], "de": ["Keine", "+3.5 dB", "+6 dB", "+9.5 dB", "+12 dB"]},d:'0'},
  ]},
  {title:{"en": "SGB Settings", "pt": "Opções SGB", "es": "Opciones SGB", "de": "SGB Optionen"},fields:[
    {k:'SGBEnableIngameHook',t:'bool',lbl:{"en": "In-game hooks", "pt": "Hooks no jogo", "es": "Hooks en juego", "de": "Im-Spiel Hooks"},d:'false'},
    {k:'SGBEnableState',t:'bool',lbl:{"en": "Savestates (XR, XL)", "pt": "Savestates (XR, XL)", "es": "Savestates (XR, XL)", "de": "Savestates (XR, XL)"},d:'false'},
    {k:'SGBEnhOverride',t:'bool',lbl:{"en": "SGB features", "pt": "Recursos do SGB", "es": "Funciones SGB", "de": "SGB Funktionen"},d:'false'},
    {k:'SGBSprIncrease',t:'bool',lbl:{"en": "Maximum sprites limit (40)", "pt": "Limite máx. sprites (40)", "es": "Límite máx. sprites (40)", "de": "Sprite-Limit (40)"},d:'false'},
    {k:'SGBClockFix',t:'bool',lbl:{"en": "Clock (Timing)", "pt": "Clock (Timing)", "es": "Clock (Timing)", "de": "Takt (Timing)"},d:'true'},
    {k:'SGBVolumeBoost',t:'select',lbl:{"en": "Volume boost", "pt": "Boost de volume", "es": "Aumento de volumen", "de": "Lautstaerke-Boost"},opts:{"en": ["None", "+3.5 dB", "+6 dB", "+9.5 dB", "+12 dB"], "pt": ["Nenhum", "+3.5 dB", "+6 dB", "+9.5 dB", "+12 dB"], "es": ["Ninguno", "+3.5 dB", "+6 dB", "+9.5 dB", "+12 dB"], "de": ["Keine", "+3.5 dB", "+6 dB", "+9.5 dB", "+12 dB"]},d:'0'},
    {k:'SGBBiosVersion',t:'num',lbl:{"en": "BIOS version", "pt": "Versão do BIOS", "es": "Versión del BIOS", "de": "BIOS-Version"},min:0,max:255,d:'2'},
  ]},
  {title:{"en": "In-game Settings", "pt": "Opções no Jogo", "es": "Opciones en Juego", "de": "Im-Spiel Optionen"},fields:[
    {k:'EnableAutoSave',t:'bool',lbl:{"en": "Autosave", "pt": "Auto-salvar", "es": "Autoguardado", "de": "Auto-Speichern"},d:'true'},
    {k:'EnableMSU1AutoSave',t:'bool',lbl:{"en": "MSU-1 Autosave", "pt": "Auto-salvar MSU-1", "es": "Autoguardado MSU-1", "de": "MSU-1 Auto-Speich."},d:'true',sub:1},
    {k:'EnableCheats',t:'bool',lbl:{"en": "Start with cheats enabled", "pt": "Iniciar com cheats", "es": "Iniciar con trucos", "de": "Mit Cheats starten"},d:'true'},
    {k:'ShortReset2Menu',t:'select',lbl:{"en": "Reset to menu", "pt": "Reset para o menu", "es": "Reset al menú", "de": "Reset zum Menue"},opts:{"en": ["Off", "Menu", "Menu + folder", "Menu + folder + ROM"], "pt": ["Deslig.", "Menu", "Menu + pasta", "Menu + pasta + ROM"], "es": ["Apag.", "Menú", "Menú + carpeta", "Menú + carpeta + ROM"], "de": ["Aus", "Menü", "Menü + Ordner", "Menü + Ordner + ROM"]},d:'0'},
    {k:'EnableIngameHook',t:'bool',lbl:{"en": "In-game hook", "pt": "Hook no jogo", "es": "Hook en juego", "de": "Im-Spiel Hook"},d:'false'},
    {k:'EnableIngameButtons',t:'bool',lbl:{"en": "In-game buttons", "pt": "Botões no jogo", "es": "Botones en juego", "de": "Im-Spiel Tasten"},d:'true',sub:1},
    {k:'EnableCheatOverlay',t:'bool',lbl:{"en": "Cheat menu", "pt": "Menu de cheats", "es": "Menú de trucos", "de": "Cheat-Menue"},d:'true',sub:1},
    {k:'EnableHookHoldoff',t:'bool',lbl:{"en": "Initial holdoff", "pt": "Espera inicial", "es": "Espera inicial", "de": "Start-Wartezeit"},d:'true',sub:1},
    {k:'R213fOverride',t:'bool',lbl:{"en": "Auto region patch", "pt": "Patch auto. de região", "es": "Patch auto. de región", "de": "Auto-Regionpatch"},d:'true'},
    {k:'1CHIPTransientFixes',t:'bool',lbl:{"en": "1CHIP transient fixes", "pt": "Correções 1CHIP", "es": "Correcciones 1CHIP", "de": "1CHIP Korrekturen"},d:'false'},
    {k:'BrightnessLimit',t:'range',lbl:{"en": "Brightness limit", "pt": "Limite de brilho", "es": "Límite de brillo", "de": "Helligkeitslimit"},min:0,max:15,d:'15'},
    {k:'ResetPatch',t:'bool',lbl:{"en": "Reset patch for clock phase", "pt": "Patch de reset (clock)", "es": "Patch de reset (clock)", "de": "Reset-Patch (Takt)"},d:'true'},
    {k:'EnableIngameSavestate',t:'bool',lbl:{"en": "In-game savestates", "pt": "Savestates no jogo", "es": "Savestates en juego", "de": "Im-Spiel Savestates"},d:'false'},
  ]},
  {title:{"en": "Savestates Settings", "pt": "Opções de Savestates", "es": "Opciones de Savestates", "de": "Savestate-Optionen"},fields:[
    {k:'EnableSavestateSlots',t:'bool',lbl:{"en": "Savestate slots", "pt": "Slots de savestate", "es": "Ranuras de savestate", "de": "Savestate-Slots"},d:'true'},
    {k:'LoadstateDelay',t:'num',lbl:{"en": "Load delay (frames)", "pt": "Atraso ao carregar", "es": "Retardo de carga", "de": "Ladeverzoegerung"},min:0,max:255,d:'10'},
  ]},
  {title:{"en": "SuperCIC Settings", "pt": "Opções SuperCIC", "es": "Opciones SuperCIC", "de": "SuperCIC Optionen"},fields:[
    {k:'PairModeAllowed',t:'bool',lbl:{"en": "Enable SuperCIC", "pt": "Ativar SuperCIC", "es": "Activar SuperCIC", "de": "SuperCIC aktivieren"},d:'false'},
    {k:'VideoModeMenu',t:'select',lbl:{"en": "Menu video mode", "pt": "Modo vídeo do menu", "es": "Modo vídeo menú", "de": "Video-Modus Menue"},opts:{"en": ["60 Hz", "50 Hz", "Auto"], "pt": ["60 Hz", "50 Hz", "Auto"], "es": ["60 Hz", "50 Hz", "Auto"], "de": ["60 Hz", "50 Hz", "Auto"]},d:'0',sub:1},
    {k:'VideoModeGame',t:'select',lbl:{"en": "Game video mode", "pt": "Modo vídeo do jogo", "es": "Modo vídeo juego", "de": "Video-Modus Spiel"},opts:{"en": ["60 Hz", "50 Hz", "Auto"], "pt": ["60 Hz", "50 Hz", "Auto"], "es": ["60 Hz", "50 Hz", "Auto"], "de": ["60 Hz", "50 Hz", "Auto"]},d:'2',sub:1},
  ]},
  {title:{"en": "Patch Options", "pt": "Opções de Patches", "es": "Opciones de Patches", "de": "Patch-Optionen"},fields:[
    {k:'PatchVerifyIntegrity',t:'bool',lbl:{"en": "Verify Integrity (~23s)", "pt": "Verificar Integridade (aprox. 23s)", "es": "Verificar Integridad (~23s)", "de": "Integritaet pruefen (~23s)"},d:'false'},
  ]},
  {title:{"en": "WiFi", "pt": "WiFi", "es": "WiFi", "de": "WiFi"},fields:[
    {k:'EnableWifi',t:'bool',lbl:{"en": "Enable WiFi", "pt": "Ativar WiFi", "es": "Activar WiFi", "de": "WLAN aktivieren"},d:'false'},
  ]},
];
function parseCfg(text){ text=text||"";
  const crlf=/\r\n/.test(text);                          // the firmware writes config.yml as CRLF
  const lines=text.split(/\r?\n/); const idx={};         // strip \r so edits stay byte-clean
  lines.forEach((ln,i)=>{ const m=ln.match(/^\s*([A-Za-z0-9_]+)\s*:/); if(m) idx[m[1]]=i; });
  return {lines,idx,crlf}; }
function cfgGet(cfg,key){ const i=cfg.idx[key]; if(i==null) return null;
  const m=cfg.lines[i].match(/:\s*(.*?)\s*(#.*)?$/); return m?m[1]:null; }
function cfgSet(cfg,key,val){ const i=cfg.idx[key];
  if(i!=null) cfg.lines[i]=cfg.lines[i].replace(/^(\s*[A-Za-z0-9_]+\s*:)\s*[^#\n]*?(\s*#.*)?$/,"$1 "+val+"$2");
  else { cfg.lines.push(key+": "+val); cfg.idx[key]=cfg.lines.length-1; } }
function cfgText(cfg){ return cfg.lines.join(cfg.crlf?"\r\n":"\n"); }   // restore the original EOL
const L = o => (o && (o[lang]||o.en)) || "";
const allCfgFields = () => [CFG_LANG, ...CFG_GROUPS.flatMap(g=>g.fields)];
let setCfg=null;
/* Called from settingsSave() AFTER the config write -- the WebUI language changes only when the
   user SAVES, not live on the dropdown. Re-skins the whole WebUI (topbar/tabs/modals via applyI18n,
   the settings form in place, other tabs rebuilt on next visit). */
function langSwitch(idx){
  lang = LANGS[idx] || "en";
  document.documentElement.lang = (lang==="pt") ? "pt-br" : lang;
  applyI18n();
  settingsRetranslate();
  // the other tabs were rendered in the old language -> rebuild them on next visit
  ["shelf","files","saves","device"].forEach(tk=>{ tabInited[tk]=0; }); tabInited.__files=0;
}
function settingsRetranslate(){
  const w=document.querySelector("#tab-settings"); if(!w) return;
  const h=w.querySelector("h2"); if(h) h.textContent=t("setTitle");
  const intro=w.querySelector(".sect-intro"); if(intro) intro.textContent=t("setIntro");
  w.querySelectorAll(".setgrp").forEach(g=>{ if(g._title) g.textContent=L(g._title); });
  w.querySelectorAll(".setrow").forEach(row=>{ const f=row._f; if(!f) return;
    const lb=row.querySelector(".setlbl"); if(lb) lb.textContent=L(f.lbl);
    if(f.t==="select"){ const sel=row.querySelector("select"); if(sel){ const v=sel.value; sel.innerHTML="";
      L(f.opts).forEach((o,i)=>{ const op=document.createElement("option"); op.value=i; op.textContent=o; sel.appendChild(op); }); sel.value=v; } }
  });
  const sv=w.querySelector(".setbar button span"); if(sv) sv.textContent=t("setSave");
}
function settingsRow(f){
  const row=document.createElement("label"); row.className="setrow"+(f.sub?" sub":""); row._f=f;
  const lb=document.createElement("span"); lb.className="setlbl"; lb.textContent=L(f.lbl);
  const cur=cfgGet(setCfg,f.k); let ctl;
  if(f.t==="bool"){ ctl=document.createElement("input"); ctl.type="checkbox"; ctl.checked=/^(true|1)$/i.test((cur!=null?cur:String(f.d)).trim()); }
  else if(f.t==="select"){ ctl=document.createElement("select");
    L(f.opts).forEach((o,i)=>{ const op=document.createElement("option"); op.value=i; op.textContent=o; ctl.appendChild(op); });
    ctl.value=String(parseInt(cur!=null?cur:f.d,10)||0); }
  else if(f.t==="num"){ ctl=document.createElement("input"); ctl.type="number"; ctl.min=f.min; ctl.max=f.max; ctl.value=cur!=null?cur:f.d; }
  else { ctl=document.createElement("input"); ctl.type="range"; ctl.min=f.min; ctl.max=f.max; ctl.value=cur!=null?cur:f.d;
    const val=document.createElement("span"); val.className="setval"; val.textContent=ctl.value; ctl.oninput=()=>val.textContent=ctl.value;
    ctl.id="set_"+f.k; row.append(lb,ctl,val); return row; }
  ctl.id="set_"+f.k; row.append(lb,ctl); return row;
}
async function settingsBuild(){
  const w=document.querySelector("#tab-settings .wrap"); w.innerHTML="";
  const head=document.createElement("div"); head.className="sect-head"; head.innerHTML="<h2></h2>"; head.firstChild.textContent=t("setTitle"); w.appendChild(head);
  const intro=document.createElement("p"); intro.className="sect-intro"; intro.textContent=t("setIntro"); w.appendChild(intro);
  let text=""; try{ const r=await fetch("/api/dl?path="+encodeURIComponent(CFG_FILE_PATH)); if(r.ok) text=await r.text(); }catch{}
  setCfg=parseCfg(text);
  const f0=document.createElement("div"); f0.className="setform"; f0.appendChild(settingsRow(CFG_LANG)); w.appendChild(f0);
  CFG_GROUPS.forEach(g=>{
    const gt=document.createElement("div"); gt.className="setgrp"; gt._title=g.title; gt.textContent=L(g.title); w.appendChild(gt);
    const form=document.createElement("div"); form.className="setform";
    g.fields.forEach(f=>form.appendChild(settingsRow(f)));
    w.appendChild(form);
  });
  const bar=document.createElement("div"); bar.className="sect-bar setbar";
  const save=document.createElement("button"); save.className="btn primary"; save.innerHTML=svg(IC.save)+"<span></span>"; save.lastChild.textContent=t("setSave"); save.onclick=settingsSave;
  bar.append(save); w.appendChild(bar);
  const st=document.createElement("div"); st.id="setStatus"; st.className="sect-status"; w.appendChild(st);
}
async function settingsSave(){
  const st=$("#setStatus");
  const known=allCfgFields().filter(f=>cfgGet(setCfg,f.k)!=null).length;
  if(known<8){ st.className="sect-status err"; st.textContent=t("setReadErr"); return; }   // read failed -> don't clobber
  allCfgFields().forEach(f=>{ const el=$("#set_"+f.k); if(!el) return;
    let v;
    if(f.t==="bool"){ const cur=(cfgGet(setCfg,f.k)||"").trim();     // some "bools" are int 0/1 in the firmware
      v = /^\d+$/.test(cur) ? (el.checked?"1":"0") : (el.checked?"true":"false"); }
    else v = el.value;
    cfgSet(setCfg, f.k, v); });
  try{ await otaPut(CFG_FILE_PATH, new TextEncoder().encode(cfgText(setCfg))); }
  catch(err){ st.className="sect-status err"; st.textContent=t("setSaveErr",err.message); return; }
  // apply the chosen language to the WebUI only now, on save (not live on dropdown change)
  const le=$("#set_Language"); if(le){ const nl=LANGS[parseInt(le.value,10)]||"en"; if(nl!==lang) langSwitch(parseInt(le.value,10)); }
  try{ const r=await fetch("/api/apply",{method:"POST"}); if(!r.ok) throw 0;   // hot-reload so the firmware re-reads (else it clobbers on its next cfg_save)
    st.className="sect-status ok"; st.textContent=t("setApplied"); }
  catch{ st.className="sect-status warn"; st.textContent=t("setApplyErr"); }
}

/* ---- Device: status, boot health, auto-boot, orphan sweeper, library audit ---- */
async function playNext(rom,patch){ const entry=patch?rom+"\t"+patch:rom; await otaPut("/sd2snes/autoboot.cfg", new TextEncoder().encode(entry+"\0")); }
async function deviceBuild(){
  const w=document.querySelector("#tab-device .wrap"); w.innerHTML="";
  const head=document.createElement("div"); head.className="sect-head"; head.innerHTML="<h2></h2>"; head.firstChild.textContent=t("devTitle"); w.appendChild(head);
  const card=document.createElement("div"); card.className="devcard";
  card.innerHTML='<div class="devrow"><span class="dk"></span><span class="dv" id="devver"></span></div>';
  card.querySelector(".dk").textContent=devName||"—"; w.appendChild(card);
  try{ const j=await(await fetch("/api/ping")).json(); if(j.ok) $("#devver").textContent=(j.fw?"fw "+j.fw+" · ":"")+"proto v"+j.ver; }catch{}
  /* primary device actions (moved here from the Files toolbar - they're device-level) */
  const acts=document.createElement("div"); acts.className="devacts";
  const mk=(ic,label,fn)=>{ const b=document.createElement("button"); b.type="button"; b.className="devact";
    b.innerHTML=svg(ic)+"<span></span>"; b.lastChild.textContent=label; b.onclick=fn; return b; };
  acts.append(mk(IC.wifi, t("wifi"), wifiOpen), mk(IC.download, t("otaBtn"), fwPick));
  w.appendChild(acts);
  const bh=document.createElement("div"); bh.className="devsec"; bh.innerHTML='<h3></h3><div class="sect-status" id="bootst"></div>'; bh.firstChild.textContent=t("devBoot"); w.appendChild(bh);
  bootHealth();
  const ab=document.createElement("div"); ab.className="devsec"; ab.innerHTML='<h3></h3><div class="devrow"><span class="dk" id="abcur"></span><button class="btn sm" id="abclr"></button></div>'; ab.firstChild.textContent=t("devAutoboot"); w.appendChild(ab);
  $("#abclr").textContent=t("devAutobootClear"); $("#abclr").onclick=async()=>{ try{ await fetch("/api/rm?path="+encodeURIComponent("/sd2snes/autoboot.cfg"),{method:"POST"}); }catch{} loadAutoboot(); };
  loadAutoboot();
  const mh=document.createElement("div"); mh.className="devmaint"; mh.innerHTML="<h3></h3>"; mh.firstChild.textContent=t("devMaint"); w.appendChild(mh);
  sweeperSection(w); auditSection(w);
}
async function bootHealth(){
  let base=false, fw=false;
  try{ const j=await(await fetch("/api/ls?path="+encodeURIComponent("/sd2snes"))).json();
    for(const e of (j.entries||[])){ if(/^fpga_base\./i.test(e.name)) base=true; if(/^firmware\.(im3|img|stm)$/i.test(e.name)) fw=true; } }catch{}
  const miss=[]; if(!base) miss.push("fpga_base"); if(!fw) miss.push("firmware.imX");
  const st=$("#bootst"); if(!miss.length){ st.className="sect-status ok"; st.textContent=t("devBootOk"); }
  else { st.className="sect-status warn"; st.textContent=t("devBootMiss",miss.join(", ")); }
}
async function loadAutoboot(){
  let entry=null; try{ const r=await fetch("/api/dl?path="+encodeURIComponent("/sd2snes/autoboot.cfg"));
    if(r.ok){ const txt=new TextDecoder().decode(new Uint8Array(await r.arrayBuffer())); entry=(txt.split("\0")[0]||"").split("\t")[0]; } }catch{}
  const ok=entry&&entry.startsWith("/");
  $("#abcur").textContent=ok?romStem(entry):t("devAutobootNone");
  $("#abclr").style.display=ok?"":"none";
}
const ROM_EXT=/\.(sfc|smc|fig|swc|bs|st|mgd|dx2|gb|gbc)$/i, ASSET_EXT=/\.(cov|srm|state|ips|bps|yml|fmv|gcv|gss)$/i;
const stemOf=n=>{ const i=n.lastIndexOf("."); return (i>0?n.slice(0,i):n).toLowerCase(); };
async function walkSD(depth,onFile){
  async function w(dir,d){ if(d>5) return;
    let j; try{ const r=await fetch("/api/ls?path="+encodeURIComponent(dir)); if(!r.ok) return; j=await r.json(); }catch{ return; }
    for(const e of (j.entries||[])){ if(hidden(e.name)) continue; const full=join(dir,e.name);
      if(e.dir) await w(full,d+1); else onFile(full,e); } }
  await w("/",0);
}
function sweeperSection(w){
  const s=document.createElement("div"); s.className="devsec"; s.innerHTML="<h3></h3>"; s.firstChild.textContent=t("sweepTitle");
  const intro=document.createElement("p"); intro.className="sect-intro"; intro.textContent=t("sweepIntro"); s.appendChild(intro);
  const bar=document.createElement("div"); bar.className="sect-bar"; const scan=document.createElement("button"); scan.className="btn"; scan.textContent=t("sweepScan"); bar.appendChild(scan); s.appendChild(bar);
  const st=document.createElement("div"); st.className="sect-status"; s.appendChild(st);
  const list=document.createElement("div"); list.className="saveslist"; s.appendChild(list); w.appendChild(s);
  scan.onclick=async()=>{ scan.disabled=true; st.textContent=t("sweepScanning",0);
    const roms=new Set(), assets=[];
    await walkSD(5,(full,e)=>{
      // never treat /sd2snes/ system files (config.yml, *.cfg, ...) as orphans; the game-info
      // assets under /sd2snes/info/ ARE game assets and stay in scope.
      if(/^\/sd2snes\//i.test(full) && !/^\/sd2snes\/info\//i.test(full)) return;
      if(ROM_EXT.test(e.name)) roms.add(stemOf(e.name));
      else if(ASSET_EXT.test(e.name)) assets.push({path:full,size:e.size,stem:stemOf(e.name)}); });
    const orphans=assets.filter(a=>!roms.has(a.stem)); scan.disabled=false;
    st.textContent=""; renderOrphans(list,st,orphans); };
}
function renderOrphans(list,st,orphans){
  list.innerHTML="";
  if(!orphans.length){ st.textContent=t("sweepNone"); return; }
  const hdr=document.createElement("div"); hdr.className="saverow hdr";
  const all=document.createElement("input"); all.type="checkbox"; all.checked=true;
  const l=document.createElement("span"); l.className="sf"; l.textContent=t("savesAll");
  const del=document.createElement("button"); del.className="btn sm danger"; del.innerHTML=svg(IC.trash)+"<span></span>"; del.lastChild.textContent=t("sweepDel");
  hdr.append(all,l,del); list.appendChild(hdr);
  orphans.forEach((o,i)=>{ const row=document.createElement("label"); row.className="saverow";
    const cb=document.createElement("input"); cb.type="checkbox"; cb.checked=true; cb.dataset.i=i;
    const nm=document.createElement("span"); nm.className="sf"; nm.textContent=o.path;
    const sz=document.createElement("span"); sz.className="ss"; sz.textContent=human(o.size);
    row.append(cb,nm,sz); list.appendChild(row); });
  all.onchange=()=>list.querySelectorAll(".saverow:not(.hdr) input").forEach(c=>c.checked=all.checked);
  del.onclick=async()=>{ const sel=[...list.querySelectorAll(".saverow:not(.hdr) input:checked")].map(c=>orphans[+c.dataset.i]);
    if(!sel.length||!confirm(t("cfmDeleteN",sel.length))) return;
    for(const o of sel){ try{ await fetch("/api/rm?path="+encodeURIComponent(o.path),{method:"POST"}); }catch{} }
    st.textContent=t("sweepDone",sel.length); list.innerHTML=""; };
}
function auditSection(w){
  const s=document.createElement("div"); s.className="devsec"; s.innerHTML="<h3></h3>"; s.firstChild.textContent=t("auditTitle");
  const bar=document.createElement("div"); bar.className="sect-bar"; const scan=document.createElement("button"); scan.className="btn"; scan.textContent=t("auditScan"); bar.appendChild(scan); s.appendChild(bar);
  const st=document.createElement("div"); st.className="sect-status"; s.appendChild(st);
  const box=document.createElement("div"); box.className="auditbox"; s.appendChild(box); w.appendChild(s);
  scan.onclick=async()=>{ scan.disabled=true; st.textContent=t("auditScanning",0);
    const roms=[], cov=new Set(), info=new Set();
    await walkSD(5,(full,e)=>{ const stem=stemOf(e.name);
      if(ROM_EXT.test(e.name)) roms.push({name:e.name,stem});
      else if(/\.cov$/i.test(e.name)) cov.add(stem); else if(/\.yml$/i.test(e.name)) info.add(stem); });
    scan.disabled=false; st.textContent=t("auditScanning",roms.length);
    box.innerHTML="";
    roms.sort((a,b)=>a.name.localeCompare(b.name)).forEach(r=>{
      const row=document.createElement("div"); row.className="auditrow";
      row.innerHTML='<span class="an"></span><span class="ax'+(cov.has(r.stem)?" y":"")+'">cov</span><span class="ax'+(info.has(r.stem)?" y":"")+'">info</span>';
      row.querySelector(".an").textContent=r.name; box.appendChild(row); }); };
}

applyI18n();   /* English defaults until the sd2snes language arrives */
(async()=>{ try{ const j=await(await fetch("/api/ping")).json();
  if(j.ok){ lang = LANGS[j.lang] || "en"; devName = j.name || ""; applyI18n();
    setDev(j.name, j.ver); $("#ver").textContent = j.fw ? "v"+j.fw : ""; }
  else setDevOff(t("mcuNo"));
}catch{ setDevOff(t("mcuNo")); } showTab(location.hash.slice(1)||"shelf"); })();
