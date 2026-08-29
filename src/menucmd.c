/* sd2snes - menu command handlers (MCU side). See menucmd.h. */

#include <string.h>
#include "config.h"
#include "uart.h"
#include "ff.h"
#include "fileops.h"
#include "fpga.h"
#include "fpga_spi.h"
#include "filetypes.h"
#include "memory.h"
#include "snes.h"
#include "cover.h"
#include "gameinfo.h"
#include "smc.h"
#include "sufami.h"
#include "cfg.h"
#include "patch.h"
#include "patchmeta.h"
#include "cheat.h"
#include "theme.h"
#include "manual.h"
#include "memtest.h"
#include "msu1.h"     /* menu_music_locked: the DAC claim menucmd_fmv_gate honours */
#include "pcmplay.h"
#include "menucmd.h"
#include "util.h"

extern volatile cfg_t CFG;
extern volatile mcu_status_t STM;
extern snes_romprops_t romprops;

/* Keeps a handler's 256-byte path buffer out of its caller's frame.  menucmd_dispatch
   sits under EVERY menu command, so a buffer the optimizer folds into it is paid on
   paths that never touch it -- and stack+heap headroom here is a couple of KB. */
#define NO_INLINE __attribute__((noinline))

/* Drop any Recent/Favorite list entries whose ROM file no longer exists, then
   re-publish both lists + status to the SNES.  Called after every ROM delete
   (browser / favorites / recents) so a now-missing game can't linger in those
   lists and hang the loader when opened.  The boot-time check (main) covers
   files removed on a PC between sessions.  cfg_validity_check rewrites a .cfg
   (keeping a .bak) only when something actually changed. */
static void revalidate_game_lists(void) {
  cfg_validity_check_listed_games(LAST_FILE);
  STM.num_recent_games = cfg_dump_listed_games_for_snes(LAST_FILE, SRAM_LASTGAME_ADDR, 1);
  cfg_validity_check_listed_games(FAVORITES_FILE);
  STM.num_favorite_games = cfg_dump_listed_games_for_snes(FAVORITES_FILE, SRAM_FAVORITEGAMES_ADDR, 0);
  status_load_to_menu();
}

/* Patch-aware Recents/Favorites: a list entry may be "<rom>\t<patch_basename>".
   Given such a raw entry, recover the patch, stage it where load_rom() expects
   it and arm ips_pending_index so the relaunch re-applies the patch (reusing the
   same path as a normal LOADROM after ips_find_patches).  `entry` is truncated to
   the bare base ROM path on return (ready to hand to load_rom).  A patch deleted
   since launch degrades to a clean vanilla boot instead of failing. */
static void stage_patch_from_entry(char *entry) {
  char patchpath[256];
  const char *pbase;
  ips_pending_index = 0;
  current_ips_srm_source[0] = '\0';
  current_ips_flags = 0;
  if(!cfg_parse_patch_entry(entry, patchpath, sizeof(patchpath))) {
    return; /* no patch tag -> plain base ROM */
  }
  FILINFO fno;
  fno.lfname = NULL;
  if(f_stat((TCHAR*)patchpath, &fno) != FR_OK) {
    printf("stage_patch: patch gone (%s) -> vanilla\n", patchpath);
    return; /* patch deleted -> boot base vanilla */
  }
  /* A path that does not fit the staging slot would be truncated and then opened
     as some OTHER file, so refuse it the same way the directory scan does. */
  if(strlen(patchpath) >= IPS_PATH_LEN) {
    printf("stage_patch: path too long (%s) -> vanilla\n", patchpath);
    return;
  }
  strlcpy_nul((char*)current_ips_srm_source, patchpath, sizeof(current_ips_srm_source));
  /* Stage the patch path where patch_apply()/bps_probe_header() read it, exactly
     as ips_find_patches() would have for a normal LOADROM (slot index 1).  No
     directory scan runs on this path, so drop whatever scan was live: its scratch
     describes some other ROM, and CMD_PATCH_META_SAVE reads basenames from it. */
  ips_scan_count = 0;
  sram_writeblock(current_ips_srm_source, SRAM_IPS_TEXT_ADDR + IPS_PATH_BASE,
                  (uint16_t)(strlen((char*)current_ips_srm_source) + 1));
  /* No directory scan happens on this path, so the header-mode override has to
     come straight from the sidecar -- `entry` is the base ROM path by now, which
     is exactly the key the sidecar is filed under. */
  pbase = path_leaf(patchpath);
  current_ips_flags = patchmeta_flags_for((const uint8_t*)entry, pbase,
                                          patch_ext_type(pbase) == PATCH_TYPE_BPS
                                            ? PATCH_TYPE_BPS : PATCH_TYPE_IPS);
  sram_writebyte(current_ips_flags, SRAM_IPS_LIST_ADDR + IPS_FLAGS_BASE);
  ips_pending_index = 1;
}

/* Resolve the Recents/Favorites entry at the index in MCU_PARAM (low byte) to
   the name its sidecar files (.srm/.yml) are keyed off: the PATCH for a
   patched entry (consistent with in-game current_ips_srm_source), the base
   ROM otherwise.  Raw entry lands in file_lfn; patchpath is caller scratch. */
static NO_INLINE char *listed_game_sidecar_source(const uint8_t *listfile) {
  /* The result is returned in the global file_lfn (not a caller stack buffer) and
     the scratch patchpath is OUR local, popped on return.  This matters: the cheat
     menu reaches cheat_yaml_load()/cheat_yaml_save() through this on the recents/
     favorites path, and those carry a large frame (cheat_record_t + yaml_token_t).
     Keeping a 256-byte buffer alive in the *caller* across that call added enough
     depth to overflow the LPC1756's ~4 KB stack into .bss (recents cheats showed
     "no cheats"/hung; the shallower browser path was fine).  Resolving here and
     returning a global keeps the cheat-load stack as shallow as the browser. */
  char patchpath[256];
  cfg_get_listed_game_raw(listfile, file_lfn,
                          listed_game_resolve_index(listfile, snes_get_mcu_param() & 0xff));
  if(cfg_parse_patch_entry((char*)file_lfn, patchpath, sizeof(patchpath))) {
    /* patched entry: the sidecar key is the patch path -- move it into file_lfn */
    strlcpy_nul((char*)file_lfn, patchpath, sizeof(file_lfn));
  }
  return (char*)file_lfn;
}

/* Where a command's target comes from: cover, info screen, autoboot, favorites,
   cheats and delete each exist in a browser flavour and one per game list, and this
   is the only thing that differs between them. */
typedef enum { SEL_BROWSER, SEL_RECENT, SEL_FAV } sel_src_t;

static const uint8_t *sel_list(sel_src_t s) {
  return (s == SEL_FAV) ? FAVORITES_FILE : LAST_FILE;
}

/* The list index the menu left in MCU_PARAM, mapped back to storage order.
   Identity for recents; only sorted favorites permute. */
static uint8_t sel_index(sel_src_t s) {
  if(s == SEL_BROWSER) return 0;   /* no list index: MCU_PARAM holds the browser selection */
  return listed_game_resolve_index(sel_list(s), snes_get_mcu_param() & 0xff);
}

/* Put the target's path in file_lfn.  `raw` keeps a patched list entry's
   "<rom>\t<patch>" tag, which the browser has no equivalent of.  For the browser
   MCU_PARAM is filled like LOADROM's params (cwd + selected entry), so
   get_selected_name works for all of these. */
static void sel_name(sel_src_t s, int raw) {
  if(s == SEL_BROWSER) {
    get_selected_name(file_lfn);
  } else if(raw) {
    cfg_get_listed_game_raw(sel_list(s), file_lfn, sel_index(s));
  } else {
    cfg_get_listed_game(sel_list(s), file_lfn, sel_index(s));
  }
}

/* The name the target's sidecar files (.srm/.yml) are keyed off.  For a list entry
   that is the PATCH of a patched game (see listed_game_sidecar_source); the browser
   has no tag to parse. */
static NO_INLINE char *sel_sidecar(sel_src_t s) {
  if(s == SEL_BROWSER) {
    get_selected_name(file_lfn);
    return (char*)file_lfn;
  }
  return listed_game_sidecar_source(sel_list(s));
}

/* Box art for the highlighted game.  load_cover is bounded + fail-safe; all three
   sources share the $C9 staging area. */
static void cover_from(sel_src_t s) {
  sel_name(s, 0);
  load_cover(file_lfn, SRAM_COVER_ADDR);
}

/* Pre-boot info screen: parse /sd2snes/info/<rom>.yml + stage cover/screenshot.
   Bounded + fail-safe and does NOT boot, so there is no NACK -- the menu polls the
   GAMEINFO status in $FF6000. */
static void gameinfo_from(sel_src_t s) {
  sel_name(s, 0);
  gameinfo_load(file_lfn);
  /* publish MANUAL_GUIDES/MANUAL_META for this game so the info screen can show the
     "X: Guides" footer and open the viewer before booting. Cached on the path: the
     screen restages on every Up/Down and a full probe is a directory pass. */
  manual_stage_meta_cached(file_lfn);
}

/* RAW so the patch tag is stored in autoboot.cfg (round-trips on NUL);
   SNES_CMD_LOAD_AUTOBOOT re-applies the patch at boot. */
static void set_autoboot_from(sel_src_t s) {
  sel_name(s, 1);
  printf("Set autoboot ROM: %s (src %d)\n", file_lfn, (int)s);
  cfg_set_autoboot_rom(file_lfn);
  STM.autoboot_enabled = 1;
  status_load_to_menu();
}

/* RAW so a patched entry carries its "<rom>\t<patch>" tag into Favorites. */
static void add_favorite_from(sel_src_t s) {
  sel_name(s, 1);
  printf("Selected name: %s (src %d)\n", file_lfn, (int)s);
  /* returns 1 ONLY when the list is full and the game is not already in it
     (0 = added, <0 = write error).  Report just the full case to the menu so
     it can show a "list full" popup; the dump+status sync below carry it. */
  STM.favorites_full = (cfg_add_listed_game(FAVORITES_FILE, file_lfn, false) == 1);
  STM.num_favorite_games = cfg_dump_listed_games_for_snes(FAVORITES_FILE, SRAM_FAVORITEGAMES_ADDR, 0);
  status_load_to_menu();
}

/* Load/save the cheat YAML.  On the list paths the SNES rewrites MCU_PARAM with the
   list index right before sending these: the cheat toggle handler clobbers it. */
static void cheats_from(sel_src_t s, int save) {
  /* No 256-byte buffer in this frame: sel_sidecar resolves into the global file_lfn,
     so this stays as shallow as the browser path. */
  char *src = sel_sidecar(s);
  printf(save ? "Save cheats for: %s (src %d)\n" : "Load cheats for: %s (src %d)\n",
         src, (int)s);
  if(save) cheat_yaml_save((uint8_t*)src);
  else     cheat_yaml_load((uint8_t*)src);
}

/* For a PATCHED list entry, delete only the patch (.ips/.bps) and drop the entry by
   index -- the base ROM is shared.  Otherwise delete the ROM; revalidate_game_lists()
   then drops it from BOTH lists by file existence.  A dead entry hangs the loader
   when opened, so the browser flavour relies on that too. */
static NO_INLINE void delete_file_from(sel_src_t s) {
  if(s == SEL_BROWSER) {
    get_selected_name(file_lfn);
  } else {
    const uint8_t *listfile = sel_list(s);
    uint8_t idx = sel_index(s);
    char patchpath[256];
    cfg_get_listed_game_raw(listfile, file_lfn, idx);
    if(cfg_parse_patch_entry((char*)file_lfn, patchpath, sizeof(patchpath))) {
      printf("Delete patch: %s (src %d)\n", patchpath, (int)s);
      if(f_unlink((TCHAR*)patchpath) != FR_OK) {
        snescmd_writebyte(0xaa, SNESCMD_SNES_CMD);
      }
      cfg_remove_listed_game(listfile, idx);
      revalidate_game_lists();
      return;
    }
  }
  printf("Delete file: %s (src %d)\n", file_lfn, (int)s);
  if(f_unlink((TCHAR*)file_lfn) != FR_OK) {
    snescmd_writebyte(0xaa, SNESCMD_SNES_CMD);
  }
  revalidate_game_lists();
}

/* Delete the battery-SRAM save(s) for `srmsrc`; the ROM/patch and the list entry stay.
   Wipes ALL slots + the .slot sidecar -- neither the browser nor the lists have a slot
   UI.  Slot 0 (legacy <stem>.srm) drives the NACK; the rest are best-effort. */
static NO_INLINE void delete_srm_for(const char *srmsrc, int src) {
  printf("Delete SRM for: %s (src %d)\n", srmsrc, src);
  for(uint8_t s = 0; s < SRM_SLOT_COUNT; s++) {
    uint8_t srmfile[256];
    char ext[8];
    srm_slot_ext(ext, sizeof(ext), s);
    /* path_asset DIRECT, not memory.c's append_save_basename: that one re-applies
       current_ips_srm_source, which survives from the last LOADED game into the menu loop and
       would make this delete the previous game's patch save instead of the selected entry. */
    /* A name too long to fit leaves the buffer EMPTY; unlinking that would either fail oddly or,
       worse on some paths, act on the wrong name. Treat it as "nothing deleted" and NACK slot 0,
       same as a failed unlink -- the menu already knows how to report that. */
    if(path_asset((char*)srmfile, sizeof(srmfile), SAVE_BASEDIR, srmsrc, ext) < 0) {
      printf("SRM path too long for %s\n", srmsrc);
      if(s == 0) snescmd_writebyte(0xaa, SNESCMD_SNES_CMD);
      continue;
    }
    printf("SRM path: %s\n", srmfile);
    FRESULT r = f_unlink((TCHAR*)srmfile);
    if(s == 0 && r != FR_OK) {
      snescmd_writebyte(0xaa, SNESCMD_SNES_CMD);
    }
  }
  { uint8_t scfile[256];
    if(path_asset((char*)scfile, sizeof(scfile), SAVE_BASEDIR, srmsrc, ".slot") >= 0)
      f_unlink((TCHAR*)scfile);
  }
}

static void delete_srm_from(sel_src_t s) {
  delete_srm_for(sel_sidecar(s), (int)s);
}

/* Stage where the browser should reopen after a menu reload, and arm the one-shot
   ST_RESTORE_BROWSER flag the SNES reads on the next boot.

   Every theme/BGM action cold-boots the menu (menu_reload -> the outer loop reloads
   m3nu.bin and resets the SNES), and clear_wram fills $7E/$7F with $55, so the menu's
   own dirlog/filesel_sel cannot survive -- only strings in BSRAM do. filesel_nav_restore
   re-navigates the path component by component from these, the same way
   filesel_nav_last does for reset-to-menu.

   `path` = full SD path of the item that was acted on (browser selections), or NULL when
   the action came from the config menu, in which case we reopen the folder the browser is
   sitting in (FILESEL_CWD, which the menu keeps at SRAM_MENU_FILEPATH_ADDR) with no item
   pre-selected. Never reuse SRAM_LASTGAME_DIR/FILE for this: those are rewritten on every
   menu boot by cfg_dump_listed_games_for_snes. */
static NO_INLINE void browser_pos_save(const char *path) {
  char dir[256];
  const char *slash = path ? strrchr(path, '/') : NULL;
  if(slash) {
    size_t dir_len = slash - path;
    if(dir_len == 0 || dir_len >= sizeof(dir)) {
      strcpy(dir, "/");
    } else {
      memcpy(dir, path, dir_len);
      dir[dir_len] = 0;
    }
    sram_writestrn((uint8_t*)dir, SRAM_BROWSER_DIR_ADDR, sizeof(dir));
    sram_writestrn((uint8_t*)(slash + 1), SRAM_BROWSER_FILE_ADDR, 256);
  } else {
    /* no path: reopen the browser's current folder, cursor left at the top */
    sram_readstrn(dir, SRAM_MENU_FILEPATH_ADDR, sizeof(dir));
    if(dir[0] != '/') strcpy(dir, "/");
    sram_writestrn((uint8_t*)dir, SRAM_BROWSER_DIR_ADDR, sizeof(dir));
    sram_writestrn((uint8_t*)"", SRAM_BROWSER_FILE_ADDR, 1);
  }
  STM.restore_browser = 1;
}

/* "Create patched ROM" (SNES_CMD_EXPORT_PATCHED_ROM).
 *
 * Lifted out of the command switch because it is a whole procedure rather than a
 * dispatch: it halts the SNES, drives a full load_rom + patch, streams the image
 * out and drags every sidecar across.
 *
 * The ROM path comes in through file_lfn, which the CALLER fills via
 * get_selected_name -- and it must do so only AFTER reading the index out of
 * MCU_PARAM, because get_selected_name goes through set_mcu_addr, which preserves
 * only the low 24 bits.
 *
 * Leaves the SNES HELD IN RESET on every path.  That is deliberate: PSRAM at
 * 0x000000 now holds the exported GAME, so releasing the CPU would boot it and let
 * it run -- reprogramming the PPU and trampling WRAM -- throughout the menu
 * teardown that follows (RTC probe, cfg_save, ...), until the reload's reset pulse
 * cut it off mid-stride.  The caller's menu_reload owns the reset from here: it
 * restores the base core, loads the menu image and issues snes_reset(1)/(0) itself.
 *
 * Returns the PATCH_EXPORT_* code to park for the reloaded browser to report.
 */
static uint8_t patch_export_command(uint8_t xidx) {
  uint8_t result = PATCH_EXPORT_FAILED;

  /* A Game Boy ROM is staged behind the SGB BIOS at a different base, so
     exporting it would write out the BIOS, not the game. */
  if(xidx >= 1 && xidx <= IPS_MAX_PATCHES && !path_is_gb((char*)file_lfn)) {
    uint32_t rom_bytes;

    ips_pending_index = xidx;
    sram_readstrn(current_ips_srm_source,
                  SRAM_IPS_TEXT_ADDR + IPS_PATH_BASE
                  + (uint32_t)(xidx - 1) * IPS_PATH_LEN,
                  sizeof(current_ips_srm_source));

    /* Refuse a name collision HERE, before the SNES is halted and before the
       multi-second load_rom+patch: re-running an export is almost always an
       accident, and the old answer -- silently minting "<...> 2.sfc" -- duplicated
       a multi-MB ROM and split its saves off under the new stem.  The SNES is
       still alive spinning in pcr_wait; the menu reload the caller issues either
       way is what recovers it, and the reloaded browser reports EXISTS.
       (PSRAM reads with the menu live are the norm; nothing here remaps.) */
    if(patch_export_exists(current_ips_srm_source)) {
      result = PATCH_EXPORT_EXISTS;
    } else {
      /* STOP THE SNES FIRST.  It is spinning inside patch_create_rom, executing the
         MENU out of PSRAM -- and load_rom reprograms the cartridge mapping for the
         game (set_rom_mask, set_mapper) LONG before its own assert_reset.  The moment
         that remap lands, the code under the spinning CPU changes and it runs off
         into whatever the new mapping exposes, scribbling over the PPU (the shredded
         header rows).  The normal boot path is immune because the SNES waits in the
         WRAM trampoline at $7EF000, off the cartridge entirely; the export has no
         trampoline, so it must halt the CPU. */
      assert_reset();

      current_ips_flags = sram_readbyte(SRAM_IPS_LIST_ADDR + IPS_FLAGS_BASE + xidx - 1);
      patch_export_size = 0;
      patch_last_ok = 0;
      rom_export_active = 1;   /* keep the SNES in reset across the dump */
      rom_bytes = load_rom(file_lfn, SRAM_ROM_ADDR, LOADROM_WITH_RESET);
      rom_export_active = 0;

      /* load_rom boots on through a failed patch by design; the export must not.
         Writing the .sfc from an unpatched (or half-patched) image hands the user a
         file that lies about what it contains -- and for a failed BPS
         patch_export_size is 0, so it would be the pristine ROM verbatim. */
      if(rom_bytes && patch_last_ok
         && patch_export_write(current_ips_srm_source,
                               SRAM_ROM_ADDR + romprops.load_address,
                               patch_export_size ? patch_export_size
                                                 : romprops.romsize_bytes)) {
        /* Bring the cover, info screen, guides, cheats and saves along, so the new
           entry is not a bare ROM the user has to re-decorate by hand.  MUST run
           before current_ips_srm_source is cleared below: the saves and states of a
           patched session are filed under the PATCH's name.
           current_filename, NOT file_lfn: load_rom's savestate slot scan calls
           cfg_get_listed_game(LAST_FILE, file_lfn, 0) (savestate.c:32), which
           overwrites that global with the TOP OF RECENTS -- so by now file_lfn
           names whatever was played last, not the ROM just loaded.  That is how a
           correctly patched Chrono Trigger once got its sidecars copied from
           another game's name.  load_rom stashes the real path in
           current_filename and nothing else touches it. */
        result = patch_export_copy_assets((uint8_t*)current_filename,
                                          current_ips_srm_source)
               ? PATCH_EXPORT_OK : PATCH_EXPORT_PARTIAL;
      }
    }
  }

  ips_pending_index = 0;
  current_ips_srm_source[0] = '\0';
  current_ips_flags = 0;
  return result;
}

/* SNES_CMD_QUERY_IPS_PATCHES: list the patches (or, for a .st, the Slot B carts) that
   go with the selected ROM.  Its own function so the 256-byte path below never lands in
   menucmd_dispatch's frame.
   Under -flto this is a tempting inline target, and LTO answers by pulling
   ips_find_patches AND patch_scan_dir in -- merging the scan's frame with qpath and
   keeping both live under patch_publish, the nesting patch.c's "three SIBLING frames"
   note warns about.  patch_scan_dir wants noinline for the same reason patch_publish
   has it; until then this is the deepest path in the file. */
static NO_INLINE void query_ips_patches(void) {
  /* MUST be its own buffer, never file_lfn: patch_scan_dir points the
     FatFs long-name buffer AT file_lfn (fno.lfname), so the first
     f_readdir would overwrite the ROM path we are scanning for -- the
     stem stops matching, the scan returns 0 patches, and the dialog
     silently never opens.  patch_publish/patchmeta_apply read the same
     pointer afterwards and would key the sidecar off whatever directory
     entry happened to be read last. */
  uint8_t qpath[256];
  get_selected_name(qpath);
  current_ips_srm_source[0] = '\0';
  current_ips_flags = 0;
  /* A Sufami Turbo minicart takes over this query: the very same dialog
     picks its Slot B companion cart, so the firmware publishes the carts
     through the patch contract and flags the list as IPS_DLGMODE_SLOTB.
     A .st never carries a patch, so nothing is lost by not scanning. */
  if(path_is_st((const char*)qpath)) {
    sufami_query_slotb(qpath);
  } else {
    ips_find_patches(qpath, SRAM_IPS_LIST_ADDR);
  }
}

/* Commands issued FROM the pre-boot info screen, i.e. the ones that must NOT stop a running FMV.
   Everything else means the SNES left that screen (see the call site in the menu loop).
     - FMV_NEXT                        : the pump itself
     - GAME_INFO / _RECENT / _FAVORITE : a different game selected while the screen stays up
     - GI_DESC_FULL                    : Y = full description, drawn OVER the still-playing clip.
                                         Stopping here killed the video AND its audio for good
                                         (nothing ever re-opens the .fmv) -- that was the bug.
     - MANUAL_S1PAGE / MANUAL_ZPAGE    : the manual viewer opened from the info screen; the same
                                         reasoning applies on the way back out of it. */
static int cmd_keeps_fmv(uint8_t cmd) {
  return cmd == SNES_CMD_FMV_NEXT
      || cmd == SNES_CMD_GAME_INFO
      || cmd == SNES_CMD_GAME_INFO_RECENT
      || cmd == SNES_CMD_GAME_INFO_FAVORITE
      || cmd == SNES_CMD_GI_DESC_FULL
      || cmd == SNES_CMD_MANUAL_S1PAGE
      || cmd == SNES_CMD_MANUAL_ZPAGE;
}

/* FMV plays only while the info screen is up. Any command the info screen itself does NOT
   issue (see cmd_keeps_fmv) means the SNES left the screen -> stop it (close the file + the
   DAC clip) so the DAC frees up for the browser's nav SFX. No-op if no FMV is active.
   (Returning to the Favorites/Recents list issues NO command -> the idle watchdog in snes.c
   covers it.) */
void menucmd_fmv_gate(uint8_t cmd) {
  /* The PCM player owns the DAC through the same menu_music_* engine, and it issues
     commands this gate does not know (play, pause, resume).  Without this guard the gate
     stopped the player's own track on its own transport command -- pressing pause killed
     the clip, and the screen then reported a read error (hardware).  Guarding on "the
     player is live" rather than listing its opcodes also keeps the gate correct for the
     CMD_PLAY_PCM that STARTS it: at that point the player is not active yet, so a real
     FMV still gets stopped, which is exactly what should happen. */
  if(menu_music_locked())
    return;
  if(!cmd_keeps_fmv(cmd))
    gameinfo_fmv_stop();
}

/* The four ways a game gets launched: the browser (LOADROM), the Recents and
   Favorites lists, and autoboot.  They differ only in where the ROM path comes from
   and how the patch is recovered -- the browser gets the patch index from the pre-boot
   dialog, the other three carry it inside the list entry -- so everything from the
   Slot B staging down is shared.

   Returns the loaded size; 0 means the load was aborted and the NACK is already out.
   The caller MUST stay in the menu loop then: dropping into in-game mode while the
   SNES is still in the menu desyncs the two. */
uint32_t menucmd_launch_rom(uint8_t cmd) {
  uint32_t filesize;
  /* Sufami Turbo Slot B pick from the boot-time selector, latched out of MCU_PARAM+7
     before get_selected_name overwrites the parameter area.  Only the browser path
     sets it; the list-driven loads leave it at SUFAMI_SEL_SIDECAR. */
  uint8_t sufami_sel = SUFAMI_SEL_SIDECAR;

  /* Start from "no patch" on every path: autoboot relies on it when the stored entry
     is empty; the other three overwrite it below. */
  ips_pending_index = 0;
  current_ips_srm_source[0] = '\0';

  if(cmd == SNES_CMD_LOADROM) {
    /* Read the IPS patch index BEFORE get_selected_name so that the
       MCU_PARAM+7 byte is not overwritten.  set_mcu_addr() only uses
       the lower 24 bits, so the index byte at offset +7 is safe. */
    ips_pending_index = snescmd_readbyte(SNESCMD_MCU_PARAM + 7);
    get_selected_name(file_lfn);
    /* For a .st minicart that index is the Slot B pick, not a patch: the
       pre-boot dialog is shared between the two jobs (see sufami_query_slotb).
       Consume it here and clear the patch index so nothing downstream tries to
       apply a patch that was never in the list. */
    if(path_is_st((const char*)file_lfn)) {
      sufami_sel = ips_pending_index;
      ips_pending_index = 0;
    }
    printf("Selected name: %s (patch idx=%d)\n", file_lfn, ips_pending_index);
    /* Build the SRM-override path from the IPS file's full SD path. */
    current_ips_flags = 0;
    if(ips_pending_index > 0 && ips_pending_index <= IPS_MAX_PATCHES) {
      sram_readstrn(current_ips_srm_source,
                    SRAM_IPS_TEXT_ADDR + IPS_PATH_BASE
                    + (uint32_t)(ips_pending_index - 1) * IPS_PATH_LEN,
                    sizeof(current_ips_srm_source));
      /* Kept in MCU RAM so a recore reload can re-stage it (see memory.c). */
      current_ips_flags = sram_readbyte(SRAM_IPS_LIST_ADDR + IPS_FLAGS_BASE
                                        + (uint32_t)(ips_pending_index - 1));
      printf("Patch SRM source: %s (flags %02x)\n", current_ips_srm_source,
             current_ips_flags);
    }
    /* Record into Recents BEFORE the load.  This DOES sit on the critical
       path -- it is SD write traffic (stream the list, write a .tmp, unlink,
       rename) and the SNES is parked in game_handshake until load_rom hands
       it the 0x55 -- but moving it after the load broke it: Recents stopped
       updating and, with it, the reset-to-menu "return to the last ROM"
       navigation that reads the same list.  The stall it costs is hidden the
       right way instead, by starting the iris as soon as the command is sent
       (snes/main.a65) rather than by shortening the MCU's work.
       For a patched launch, store "<rom>\t<patch_basename>" so the list
       shows/relaunches the patch (cfg_add_listed_game_patched appends the tag
       with bounded strncat and dedups on the whole string; cwd qualification
       still applies to the leading ROM part, and an over-long entry degrades
       to base-only inside the helper). */
    if(current_ips_srm_source[0]) {
      const char *pbase = path_leaf((const char*)current_ips_srm_source);
      cfg_add_listed_game_patched(LAST_FILE, file_lfn, pbase, true);
    } else {
      cfg_add_listed_game(LAST_FILE, file_lfn, true);
    }
  } else {
    if(cmd == SNES_CMD_LOAD_AUTOBOOT) {
      cfg_get_autoboot_rom(file_lfn);
      printf("Autobooting: %s\n", file_lfn);
      if(!file_lfn[0]) {
        /* nothing stored: NACK so autoboot_cmd_handshake returns cleanly to the
           menu.  Clear the file error state too, or the LED blinks. */
        file_res = FR_OK;
        snescmd_writebyte(0xaa, SNESCMD_SNES_CMD);
        return 0;
      }
    } else {
      /* read RAW so the "<rom>\t<patch>" tag survives the move-to-top */
      const uint8_t *list = (cmd == SNES_CMD_LOADFAVORITE) ? FAVORITES_FILE : LAST_FILE;
      cfg_get_listed_game_raw(list, file_lfn,
                              listed_game_resolve_index(list, snes_get_mcu_param() & 0xff));
      printf("Selected name: %s\n", file_lfn);
    }
    /* RAW entry, tag intact -- a favorite lands in recents too */
    cfg_add_listed_game(LAST_FILE, file_lfn, true);
    /* re-applies the patch load_rom is about to want; truncates file_lfn to base */
    stage_patch_from_entry((char*)file_lfn);
  }

  /* MUST run before load_rom: it is what resolves and stages the Slot B path
     the load then reads.  Also persists the A+B pair to the .stb sidecar. */
  sufami_stage_slotb(file_lfn, sufami_sel);
  filesize = load_rom(file_lfn, SRAM_ROM_ADDR, LOADROM_WITH_SRAM | LOADROM_WITH_RESET | LOADROM_WAIT_SNES);
  if(filesize) return filesize;   /* ROM loaded and SNES reset, leave the menu loop */
  /* load aborted (missing chip BIOS etc.): clear the file error state -- or the LED
     blinks -- and NACK, so game_handshake_error shows the message. */
  file_res = FR_OK;
  /* Republish Recents.  The write above happens BEFORE the pre-check, so a refused game
     has already been moved to the top of lastgame.cfg while the SNES-side mirror still
     holds the old order -- and the menu resolves a cover, a game-info query or a retry by
     INDEX into the list the MCU reads.  Left stale, the row the user sees and the entry
     the MCU acts on are different games (the refused .nes shows up as the title below it,
     its cover and its info screen).  Only the dump: the entries themselves are fine, so
     there is nothing to validity-check. */
  STM.num_recent_games = cfg_dump_listed_games_for_snes(LAST_FILE, SRAM_LASTGAME_ADDR, 1);
  status_load_to_menu();
  snescmd_writebyte(0xaa, SNESCMD_SNES_CMD);
  return 0;
}

uint8_t menucmd_dispatch(uint8_t cmd, uint8_t *menu_reload) {
  switch(cmd) {
    case SNES_CMD_QUERY_IPS_PATCHES:
      query_ips_patches();
      return 0;
    case SNES_CMD_PATCH_META_SAVE:
      /* The menu edited the header-mode field of the staged flags bytes; rewrite
         the sidecar from the scan we still hold (which also prunes entries whose
         patch has since been deleted).  patchmeta_save pairs each staged flags
         byte with its basename through patch_basename_at, so nothing has to be
         folded back by hand here.  Guard on the scan being live so a stale one
         can never be written out under some other ROM's name. */
      if(ips_scan_count) {
        get_selected_name(file_lfn);
        patchmeta_save(file_lfn, SRAM_IPS_LIST_ADDR, ips_scan_count);
      }
      snescmd_writebyte(0x55, SNESCMD_SNES_CMD);
      return 0;
    case SNES_CMD_EXPORT_CHECK: {
      /* Pre-flight for the export below: answer "would it collide?" while the
         menu is still fully alive, so a refusal is a modal over the live patch
         dialog instead of a screen teardown + cold boot.  Index read BEFORE
         get_selected_name for the same set_mcu_addr reason as the export. */
      uint8_t cidx = snescmd_readbyte(SNESCMD_MCU_PARAM + 7);
      uint8_t cres = PATCH_EXPORT_NONE;
      get_selected_name(file_lfn);
      if(cidx >= 1 && cidx <= IPS_MAX_PATCHES && !path_is_gb((char*)file_lfn)) {
        sram_readstrn(current_ips_srm_source,
                      SRAM_IPS_TEXT_ADDR + IPS_PATH_BASE
                      + (uint32_t)(cidx - 1) * IPS_PATH_LEN,
                      sizeof(current_ips_srm_source));
        /* patch_export_exists also stages the existing path in
           SRAM_EXPORT_PATH_ADDR; unread on this path, needed on the fallback. */
        if(patch_export_exists(current_ips_srm_source))
          cres = PATCH_EXPORT_EXISTS;
        current_ips_srm_source[0] = '\0';
      }
      /* Persistent answer first, $55 after: the menu waits on the $55 and then
         reads EXPORT_RESULT (and one-shots it) -- no transient-NACK race. */
      sram_writebyte(cres, SRAM_EXPORT_RESULT_ADDR);
      snescmd_writebyte(0x55, SNESCMD_SNES_CMD);
      return 0;
    }
    case SNES_CMD_EXPORT_PATCHED_ROM: {
      /* Read the index BEFORE get_selected_name, same as LOADROM: that call
         goes through set_mcu_addr, which only preserves the low 24 bits.
         It has to be its own statement, not an argument alongside the call --
         C does not order argument evaluation. */
      uint8_t xidx = snescmd_readbyte(SNESCMD_MCU_PARAM + 7);
      get_selected_name(file_lfn);
      /* The SNES is in reset and cannot read a handshake byte, so the outcome is
         parked here and the reloaded browser pops a modal for it -- the same
         modal machinery the missing-chip-BIOS message uses. */
      sram_writebyte(patch_export_command(xidx), SRAM_EXPORT_RESULT_ADDR);
      /* The menu image in PSRAM was overwritten by the game load either way,
         so the menu has to be cold-booted back; that is also what makes the
         new file show up in the browser listing.  NOTE: returns cmd, not 0 --
         menu_reload only takes effect once the menu loop exits (see SET_THEME). */
      *menu_reload = 1;
      return cmd;
    }
    case SNES_CMD_MEMTEST: {
      /* "Memory test": walk the RAM wiring and report faults.  Two very different
         answers, and which one it is decides whether the SNES survives the command.
         (1) No fpga_test on the card -> nothing has happened yet, so refuse IN PLACE:
             park the code, ACK, and the menu pops a modal over the live screen.  The
             answer rides the persistent block rather than an $aa on SNES_CMD, the same
             race SRAM_LOAD_NACK_ADDR exists for (the menu loop re-arms $55 immediately).
         (2) Otherwise the test reconfigures the FPGA out from under the running menu, so
             HALT THE SNES FIRST -- it is executing the menu out of PSRAM, and the moment
             the test core comes up that code is gone.  Same reasoning as the patched-ROM
             export, minus its trampoline: there is nowhere off-cartridge to park the CPU.
         The result then has to survive to the next boot, which it does: memtest_run
         publishes into $FF07xx and nothing on the reload path writes there. */
      /* Which of the two tests the menu asked for.  Read in a statement of its own and
         BEFORE anything that touches the FPGA address latch, exactly like the xidx of
         EXPORT_PATCHED_ROM above: snescmd_readbyte goes through set_mcu_addr, so folding
         it into a later expression makes the order the compiler's business. */
      uint8_t mtmode = snescmd_readbyte(SNESCMD_MCU_PARAM + 7);
      if(!memtest_available()) {
        memtest_publish_nocore();
        snescmd_writebyte(0x55, SNESCMD_SNES_CMD);
        return 0;
      }
      assert_reset();
      memtest_run(mtmode);
      /* NOTE: returns cmd, not 0 -- menu_reload only takes effect once the menu loop
         exits.  The reload is not optional: the FPGA is on the test core, the low PSRAM
         is scribbled over, and the SNES is in reset.  main()'s outer loop puts fpga_base
         back and re-stages the menu image. */
      *menu_reload = 1;
      return cmd;
    }
    case SNES_CMD_PLAY_PCM:
      /* A .pcm was picked in the browser: play it on the cartridge DAC.  MCU_PARAM was
         set up like a ROM launch (cwd + selected entry), so get_selected_name yields the
         full SD path.  Nothing here boots or reloads -- the menu stays live and the
         player screen (snes/pcmplay.a65) reads PCMPLAY_BLK, which the MCU republishes on
         its own once per menu-loop pass.  Fail-safe: a missing / non-"MSU1" file only
         publishes an error state, so a bad file costs a message, not a hang. */
      get_selected_name(file_lfn);
      pcmplay_start((const char*)file_lfn);
      return 0;
    case SNES_CMD_PCM_CTL:
      /* Transport for the player above; MCU_PARAM low byte = PCM_CTL_*.  Pause freezes
         the DAC read pointer only: the file stays open at its position, which is what
         makes resuming instant (see pcmplay_pause). */
      switch(snescmd_readbyte(SNESCMD_MCU_PARAM) ) {
        case PCMPLAY_CTL_PAUSE:  pcmplay_pause(1); break;
        case PCMPLAY_CTL_RESUME: pcmplay_pause(0); break;
        default:                 pcmplay_stop();   break;
      }
      return 0;
    case SNES_CMD_LOAD_MENU_SPC:
      /* stage background menu music. Use the user-chosen .spc (CFG.bgm_name, a
         full SD path set via SNES_CMD_SET_MENU_SPC) when present, otherwise fall
         back to the fixed /sd2snes/menu.spc. load_spc is graceful: a missing/
         too-small file zeroes the SPC header, which the menu detects and skips. */
      load_spc((uint8_t*)(CFG.bgm_name[0] == '/' ? CFG.bgm_name : (uint8_t*)"/sd2snes/menu.spc"),
               SRAM_SPC_DATA_ADDR, SRAM_SPC_HEADER_ADDR);
      return 0;
    case SNES_CMD_ADD_FAVORITE_ROM:
      add_favorite_from(SEL_BROWSER);
      return 0;
    case SNES_CMD_ADD_FAVORITE_RECENT:
      add_favorite_from(SEL_RECENT);
      return 0;
    case SNES_CMD_SET_AUTOBOOT_ROM:
      set_autoboot_from(SEL_BROWSER);
      return 0;
    case SNES_CMD_SET_AUTOBOOT_FAV:
      set_autoboot_from(SEL_FAV);
      return 0;
    case SNES_CMD_SET_AUTOBOOT_RECENT:
      set_autoboot_from(SEL_RECENT);
      return 0;
    case SNES_CMD_DELETE_FILE:
      delete_file_from(SEL_BROWSER);
      return 0;
    case SNES_CMD_DELETE_SRM:
      delete_srm_from(SEL_BROWSER);
      return 0;
    case SNES_CMD_DELETE_FILE_FAV:
      delete_file_from(SEL_FAV);
      return 0;
    case SNES_CMD_DELETE_SRM_FAV:
      delete_srm_from(SEL_FAV);
      return 0;
    case SNES_CMD_DELETE_FILE_RECENT:
      delete_file_from(SEL_RECENT);
      return 0;
    case SNES_CMD_DELETE_SRM_RECENT:
      delete_srm_from(SEL_RECENT);
      return 0;
    case SNES_CMD_LOAD_COVER:
      cover_from(SEL_BROWSER);
      return 0;
    case SNES_CMD_LOAD_COVER_RECENT:
      cover_from(SEL_RECENT);
      return 0;
    case SNES_CMD_LOAD_COVER_FAVORITE:
      cover_from(SEL_FAV);
      return 0;
    case SNES_CMD_GAME_INFO:
      gameinfo_from(SEL_BROWSER);
      return 0;
    case SNES_CMD_GAME_INFO_RECENT:
      gameinfo_from(SEL_RECENT);
      return 0;
    case SNES_CMD_GAME_INFO_FAVORITE:
      gameinfo_from(SEL_FAV);
      return 0;
    case SNES_CMD_FMV_NEXT:
      /* info-screen FMV pump: stream the next <rom>.fmv frame into the band tile
         bank ($CA0000). Bounded + fail-safe (no-op if no .fmv open); does NOT boot. */
      gameinfo_fmv_next();
      return 0;
    case SNES_CMD_GI_DESC_FULL:
      /* "full description" (Y) on the info screen: re-scan the last-loaded .yml and
         stage the COMPLETE (untruncated) description into $FF7600. Bounded + fail-safe
         (on any error the region stays invalid -> menu keeps the 256-char copy); no boot. */
      gameinfo_desc_full();
      return 0;
    case SNES_CMD_MANUAL_ZPAGE:
    case SNES_CMD_MANUAL_S1PAGE:
      /* Manual viewer opened from the PRE-BOOT info screen: the same commands the
         in-game GUIDES tab issues, served by the shared dispatcher in snes.c.
         Nothing calls snes_set_mcu_cmd(0) here -- in the menu the ACK is the MCU_CMD
         clear at the TOP of the next menu_main_loop() iteration, which is what the
         viewer's bounded "spin until MCU_CMD == 0" waits for.  So: return 0. */
      game_cmd_serve(cmd);
      return 0;
    case SNES_CMD_SET_THEME:
      /* a .thm was picked in the browser (any visible folder). MCU_PARAM was
         set up like LOADROM (cwd + selected entry) so get_selected_name
         yields the full SD path; store it and reload the menu so theme_apply
         patches the gfxptr regions of the fresh menu image. */
      get_selected_name(file_lfn);
      theme_select((char*)file_lfn);
      browser_pos_save((char*)file_lfn); /* come back to this .thm, not to the root */
      *menu_reload = 1; /* leave loop -> outer loop reloads + themes the menu */
      return cmd;
    case SNES_CMD_CLR_THEME:
      /* revert to the baked-in default look */
      theme_select(NULL);
      browser_pos_save(NULL);
      *menu_reload = 1;
      return cmd;
    case SNES_CMD_RESTORE_CLASSIC:
      /* "Restore classic theme": apply the pre-2.16 look, which now ships as a
         regular theme file instead of being the baked default.
         f_stat first: the common update path is "copy firmware + m3nu.bin" without
         refreshing /sd2snes, and writing a skin_name that points at a missing file
         would persist in config.yml while the menu reloaded to no visible effect.
         On miss: leave CFG.skin_name alone, NACK, and stay in the menu loop so the
         SNES pops an error instead of reloading. */
      if(f_stat((const TCHAR*)THEME_CLASSIC, NULL) == FR_OK) {
        theme_select(THEME_CLASSIC);
        browser_pos_save(NULL);
        *menu_reload = 1;
      } else {
        printf("theme: %s not found on card\n", THEME_CLASSIC);
        snes_menu_errmsg(MENU_ERR_SUPPLFILE, THEME_CLASSIC);
        snescmd_writebyte(0xaa, SNESCMD_SNES_CMD);
        return 0;
      }
      return cmd;
    case SNES_CMD_SET_MENU_SPC:
      /* a .spc was picked in the browser (any visible folder) to become the menu
         background music. MCU_PARAM was set up like LOADROM (cwd + selected entry)
         so get_selected_name yields the full SD path; store it, enable music, and
         persist, then reload the menu (like SET_THEME). The cold reload re-syncs
         SRAM via cfg_load_to_menu and starts the new BGM cleanly on boot -- the
         only reliable way to (re)start the S-SMP (the in-place warm-reset path
         black-screened: the warm boot leaves NMI off). */
      get_selected_name(file_lfn);
      /* strncpy for the zero padding: the whole cfg_t goes to the shared BSRAM,
         tail included (see CK_STR in cfg.c). */
      strncpy((char*)CFG.bgm_name, (char*)file_lfn, sizeof(CFG.bgm_name) - 1);
      CFG.bgm_name[sizeof(CFG.bgm_name) - 1] = 0;
      CFG.enable_menu_music = 1;
      cfg_save();
      browser_pos_save((char*)file_lfn); /* come back to this .spc */
      *menu_reload = 1; /* leave loop -> outer loop reloads, boots into the new BGM */
      return cmd;
    case SNES_CMD_CLR_MENU_SPC:
      /* "Restore music": drop the chosen .spc so the BGM falls back to
         /sd2snes/menu.spc, then reload the menu (like CLR_THEME). */
      CFG.bgm_name[0] = 0;
      cfg_save();
      browser_pos_save(NULL);
      *menu_reload = 1;
      return cmd;
    case SNES_CMD_LOAD_CHT:
      cheats_from(SEL_BROWSER, 0);
      return 0;
    case SNES_CMD_SAVE_CHT:
      cheats_from(SEL_BROWSER, 1);
      return 0;
    case SNES_CMD_LOAD_CHT_FAV:
      cheats_from(SEL_FAV, 0);
      return 0;
    case SNES_CMD_SAVE_CHT_FAV:
      cheats_from(SEL_FAV, 1);
      return 0;
    case SNES_CMD_LOAD_CHT_RECENT:
      cheats_from(SEL_RECENT, 0);
      return 0;
    case SNES_CMD_SAVE_CHT_RECENT:
      cheats_from(SEL_RECENT, 1);
      return 0;
    case SNES_CMD_TOGGLE_CHT: {
      /* toggle the enabled flag for the cheat at the index passed
         in MCU_PARAM low two bytes (16-bit index, supports 0..511).
         The MCU does the bit flip directly in the PSRAM cheat
         record at $D00000+512*idx because the SNES menu mapper
         makes that region read-only. */
      uint32_t idx = snes_get_mcu_param() & 0xffff;
      printf("Toggle cheat idx=%lu\n", (unsigned long)idx);
      cheat_toggle_flag((int)idx);
      return 0;
    }
    case SNES_CMD_RESET_TO_MENU:
      /* USB-triggered menu reload: leave the menu loop so the outer loop
         re-runs load_rom(MENU_FILENAME) and reboots into the fresh menu.bin.
         Lets menu.bin be updated over USB without a physical power-cycle. */
      *menu_reload = 1;
      return cmd;
    default:
      printf("unknown cmd: %d\n", cmd);
      break;
  }
  return 0; /* unknown cmd: stay in loop */
}

NO_INLINE void menucmd_export_boot_nav(uint8_t firstboot) {
  /* A just-finished "create patched ROM" wants the browser to open ON the new file.
     This has to run AFTER the recents/favorites dump in main(), which rewrites
     both of those from the recents list.  Reuses the reset-to-menu navigation (filesel_nav_last), which
     walks LASTGAME_DIR component by component and then selects LASTGAME_FILE. */
  /* PSRAM comes up with whatever the last power-on left in it -- nothing zeroes
     $FF07xx -- so clear this byte ONCE, or garbage in it pops "Cannot write
     patched ROM" on a cold start with no export in sight.
     It has to be HERE, not up next to fpga_init(): sram_writebyte drives the FPGA
     memory window and blocks in an UNBOUNDED FPGA_WAIT_RDY, so before fpga_pgm()
     has configured the FPGA that call never returns -- no menu, no USB, dead
     console.  And it has to be gated on firstboot: a real export writes this byte
     and then asks for a menu reload, which re-enters main()'s outer loop, and
     the reloaded browser is exactly who consumes it. */
  if(firstboot) sram_writebyte(PATCH_EXPORT_NONE, SRAM_EXPORT_RESULT_ADDR);
  /* Same story, same window: the memory-test block is persistent PSRAM, so a cold start
     would otherwise render whatever the last power-on left there as a result screen.
     Cleared only on firstboot for the same reason as the byte above -- a real run writes
     this block and THEN asks for the reload that is supposed to display it. */
  /* Same for the PCM player's block: leftover PSRAM here would have the browser think a
     track is playing on a freshly powered-on console. */
  if(firstboot) {
    memtest_clear();
    pcmplay_clear();
  }
  /* OK *and* PARTIAL: a partial export still WROTE the .sfc -- only some sidecar
     failed to copy -- and export_result_check (snes/patch.a65) navigates the
     browser to it in both cases.  Testing == PATCH_EXPORT_OK here left LASTGAME_
     DIR/FILE pointing at the previous entry, so a partial export walked the user
     to the top of Recents instead of to the file just created.
     EXISTS too (the belt-and-braces refusal inside the export, when a file
     appeared between the pre-flight CMD_EXPORT_CHECK and the run): there the
     staged path is the EXISTING .sfc, and landing on it beats dumping the user
     at the top of a fresh browser. */
  uint8_t xres = sram_readbyte(SRAM_EXPORT_RESULT_ADDR);
  if(xres == PATCH_EXPORT_OK || xres == PATCH_EXPORT_PARTIAL
     || xres == PATCH_EXPORT_EXISTS) {
    uint8_t xpath[256];
    sram_readstrn(xpath, SRAM_EXPORT_PATH_ADDR, sizeof(xpath));
    char *xslash = strrchr((char*)xpath, '/');
    if(xslash) {
      sram_writestrn((uint8_t*)(xslash + 1), SRAM_LASTGAME_FILE_ADDR, 256);
      if(xslash == (char*)xpath) {
        sram_writestrn((uint8_t*)"/", SRAM_LASTGAME_DIR_ADDR, 256);
      } else {
        *xslash = 0;
        sram_writestrn(xpath, SRAM_LASTGAME_DIR_ADDR, 256);
      }
    }
  }
}
