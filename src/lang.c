#include <stddef.h>
#include "lang.h"
#include "cfg.h"

extern cfg_t CFG;

uint8_t lang_idx(void) {
  return CFG.language < NUM_LANG ? CFG.language : 0;
}

/* CIC "friendly" state names (cic.c) -------------------------------------- */
const char *const cicstatefriendly_l[NUM_LANG][4] = {
  /* LANG_EN   */ { "Original or no CIC", "Original CIC (failed)", "SuperCIC enhanced", "SuperCIC detected, not used" },
  /* LANG_PTBR */ { "Original ou sem", "CIC original (falhou)", "SuperCIC ampliado", "SuperCIC detect. n/usado" },
  /* LANG_ES   */ { "Original o sin CIC", "CIC original (fall\x89)", "SuperCIC ampliado", "SuperCIC detect. sin uso" },
  /* LANG_DE   */ { "Original/kein CIC", "Original CIC (Fehler)", "SuperCIC erweitert", "SuperCIC erkannt, ungenutzt" },
};

/* FatFs FRESULT friendly names (fileops.c) -------------------------------- */
const char *const fresult_friendly_names_l[NUM_LANG][20] = {
  /* LANG_EN */ {
    "No error", "Card I/O error", "Internal FS driver error",
    "Drive not ready", "File not found", "Directory not found", "Invalid path name",
    "Access denied", "Access denied (exists)", "Invalid file object", "Write protected",
    "Invalid drive specified", "No work area", "Not a valid file system", "mkfs() aborted",
    "Drive access timeout", "Shared access locked", "Not enough memory", "Too many open files",
    "Invalid parameter"
  },
  /* LANG_PTBR */ {
    "Sem erro", "Erro de I/O", "Erro interno FS",
    "Drive nao pronto", "Arq. nao achado", "Pasta nao achada", "Caminho invalido",
    "Acesso negado", "Negado (ja existe)", "Arq. invalido", "Protegido",
    "Drive invalido", "Sem mem. trab.", "FS invalido", "mkfs() abortou",
    "Timeout no SD", "Acesso bloqueado", "Sem memoria", "Muitos arq. abertos",
    "Param. invalido"
  },
  /* LANG_ES */ {
    "Sin error", "Error de E/S", "Error interno FS",
    "Disco no listo", "Arch. no hallado", "Carpeta no hallada", "Ruta inv\x82lida",
    "Acceso denegado", "Denegado (ya existe)", "Arch. inv\x82lido", "Protegido",
    "Disco inv\x82lido", "Sin \x82rea trab.", "FS inv\x82lido", "mkfs() abortado",
    "Timeout del SD", "Acceso bloqueado", "Sin memoria", "Muchos arch. abiertos",
    "Par\x82m. inv\x82lido"
  },
  /* LANG_DE */ {
    "Kein Fehler", "Karten-I/O Fehler", "Interner FS-Fehler",
    "Laufwerk nicht bereit", "Datei nicht gefunden", "Ordner nicht gefunden", "Ungueltiger Pfad",
    "Zugriff verweigert", "Verweigert (existiert)", "Ungueltiges Objekt", "Schreibgeschuetzt",
    "Ungueltiges Laufwerk", "Kein Arbeitsbereich", "Kein gueltiges FS", "mkfs() abgebrochen",
    "Laufwerk Timeout", "Zugriff gesperrt", "Nicht genug Speicher", "Zu viele offene Dateien",
    "Ungueltiger Parameter"
  },
};

/* sysinfo screen messages (sysinfo.c) ------------------------------------- */
const char *const sysinfo_msg[NUM_LANG][SI_COUNT] = {
  /* LANG_EN */ {
    [SI_BUSY_DISK]          = "Calculating disk space\x7f\x80                ",
    [SI_FW_VERSION]         = "    Firmware version: %s",
    [SI_SD_REMOVED]         = "    *** SD Card removed/USB busy ***    ",
    [SI_SD_MAKER]           = "SD Maker/OEM:    0x%02x, \"%c%c\"",
    [SI_SD_PRODUCT]         = "SD Product Name: \"%c%c%c%c%c\", Rev. %d.%d",
    [SI_SD_SERIAL]          = "SD Serial No.:   %02x%02x%02x%02x, Mfd. %d/%02d",
    [SI_SD_ACC_TIME]        = "SD acc. time: %ld.%03ld / %ld.%03ld ms avg/max",
    [SI_SD_ACC_MEASURING]   = "SD acc. time: measuring\x7f\x80  ",
    [SI_CARD_USAGE]         = "Card usage: %ldMB / %ldMB",
    [SI_CIC_STATE]          = "CIC state: %s",
    [SI_SNES_CLK_MEASURING] = "SNES master clock: measuring\x7f\x80",
    [SI_SNES_CLK]           = "SNES master clock: %ldHz    ",
    [SI_COMPANION]          = "Companion: %s",
    [SI_NOT_DETECTED]       = "Not detected",
  },
  /* LANG_PTBR */ {
    [SI_BUSY_DISK]          = "Calculando espa\x8do em disco\x7f\x80            ",
    [SI_FW_VERSION]         = "    Vers\x85o firmware: %s",
    [SI_SD_REMOVED]         = "    *** SD removido/USB ocupado ***    ",
    [SI_SD_MAKER]           = "Fabricante SD:   0x%02x, \"%c%c\"",
    [SI_SD_PRODUCT]         = "Nome do produto: \"%c%c%c%c%c\", Rev. %d.%d",
    [SI_SD_SERIAL]          = "Serial SD:       %02x%02x%02x%02x, Fab. %d/%02d",
    [SI_SD_ACC_TIME]        = "Acesso SD:    %ld.%03ld / %ld.%03ld ms med/max",
    [SI_SD_ACC_MEASURING]   = "Acesso SD:    medindo\x7f\x80",
    [SI_CARD_USAGE]         = "Uso do SD:  %ldMB / %ldMB",
    [SI_CIC_STATE]          = "Modo CIC: %s",
    [SI_SNES_CLK_MEASURING] = "Clock SNES: medindo\x7f\x80",
    [SI_SNES_CLK]           = "Clock SNES: %ldHz",
    [SI_COMPANION]          = "Companion: %s",
    [SI_NOT_DETECTED]       = "N\x85o detectado",
  },
  /* LANG_ES */ {
    [SI_BUSY_DISK]          = "Calculando espacio en disco\x7f\x80           ",
    [SI_FW_VERSION]         = "    Versi\x89n firmware: %s",
    [SI_SD_REMOVED]         = "   *** SD extra\x88""do/USB ocupado ***   ",
    [SI_SD_MAKER]           = "Fabricante SD:   0x%02x, \"%c%c\"",
    [SI_SD_PRODUCT]         = "Nombre producto: \"%c%c%c%c%c\", Rev. %d.%d",
    [SI_SD_SERIAL]          = "Serial SD:       %02x%02x%02x%02x, Fab. %d/%02d",
    [SI_SD_ACC_TIME]        = "Acceso SD:    %ld.%03ld / %ld.%03ld ms med/m\x82x",
    [SI_SD_ACC_MEASURING]   = "Acceso SD:    midiendo\x7f\x80",
    [SI_CARD_USAGE]         = "Uso del SD: %ldMB / %ldMB",
    [SI_CIC_STATE]          = "Estado CIC: %s",
    [SI_SNES_CLK_MEASURING] = "Clock SNES: midiendo\x7f\x80",
    [SI_SNES_CLK]           = "Clock SNES: %ldHz",
    [SI_COMPANION]          = "Companion: %s",
    [SI_NOT_DETECTED]       = "No detectado",
  },
  /* LANG_DE */ {
    [SI_BUSY_DISK]          = "Berechne Speicherplatz\x7f\x80              ",
    [SI_FW_VERSION]         = "    Firmware-Version: %s",
    [SI_SD_REMOVED]         = "    *** SD entfernt/USB belegt ***    ",
    [SI_SD_MAKER]           = "SD Hersteller:   0x%02x, \"%c%c\"",
    [SI_SD_PRODUCT]         = "SD Produktname: \"%c%c%c%c%c\", Rev. %d.%d",
    [SI_SD_SERIAL]          = "SD Seriennr.:   %02x%02x%02x%02x, Herst. %d/%02d",
    [SI_SD_ACC_TIME]        = "SD Zugriff:  %ld.%03ld / %ld.%03ld ms avg/max",
    [SI_SD_ACC_MEASURING]   = "SD Zugriff:  messe\x7f\x80",
    [SI_CARD_USAGE]         = "SD Nutzung: %ldMB / %ldMB",
    [SI_CIC_STATE]          = "CIC Status: %s",
    [SI_SNES_CLK_MEASURING] = "SNES Takt: messe\x7f\x80",
    [SI_SNES_CLK]           = "SNES Takt: %ldHz",
  },
};

/* SGB BIOS state words (sysinfo.c) ---------------------------------------- */
const char *const sgb_state_l[NUM_LANG][SGB_W_COUNT] = {
  /* LANG_EN   */ { "missing", "mismatch", "ok", "checking" },
  /* LANG_PTBR */ { "ausente", "errado", "ok", "lendo" },
  /* LANG_ES   */ { "ausente", "err\x89neo", "ok", "leyendo" },
  /* LANG_DE   */ { "fehlt", "falsch", "ok", "pruefe" },
};

const char *const cheatmenu_l[NUM_LANG][CHEATMENU_COUNT] = {
  /* LANG_EN   */ { "Cheats for ", "(no name)" },
  /* LANG_PTBR */ { "Cheats para ", "(sem nome)" },
  /* LANG_ES   */ { "Trucos para ", "(sin nombre)" },
  /* LANG_DE   */ { "Cheats fuer ", "(kein Name)" },
};
