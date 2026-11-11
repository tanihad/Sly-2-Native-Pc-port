/* Signed release manifests and the game folder swap (launcher updates). Pure logic, no UI: unit-tested on
 * its own (tests/unit/launcher). See release.c and notes/install.md "Updates".
 *
 * A release is three files (scripts/make_release.py):
 *   sly2-release.txt        the manifest, "key = value" lines (UTF-8, '#' comments)
 *   sly2-release.txt.sig    Ed25519 signature of the manifest's exact bytes, 128 hex digits
 *   sly2-source-<ver>.zip   the source bundle (what Setup carries as source.zip)
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#define RELEASE_PRODUCT "sly2-pc"
#define RELEASE_FORMAT 1
#define RELEASE_MAX_MANIFEST 16384          /* bytes: a manifest is a few hundred */
#define RELEASE_MAX_BUNDLE (256ull << 20)   /* bytes: a bundle is about 1 MB */

typedef struct {
    char version[32];                       /* "0.1.1300" */
    char commit[48];
    char date[32];
    char bundle[1024];                      /* URL (https://...) or a name relative to the manifest's URL */
    uint64_t bundle_size;
    uint8_t bundle_sha256[32];
    char min_launcher[32];                  /* oldest launcher that can install it ("" = any) */
    char page[512];                         /* release page (shown when a newer launcher is needed) */
    char notes[512];
} release_manifest;

/* Parses a manifest. 1 = ok; 0 = invalid, why says what. Unknown keys are ignored (newer manifests). */
int release_parse(const char* text, size_t n, release_manifest* m, char* why, size_t cap);

/* 1 = sig_text (128 hex digits, whitespace around it allowed) is a valid Ed25519 signature of text by pub. */
int release_verify(const char* text, size_t n, const char* sig_text, size_t sig_n, const uint8_t pub[32]);

/* Dotted numeric versions: <0, 0, >0 like strcmp; a malformed version sorts below every valid one. */
int release_version_cmp(const char* a, const char* b);

/* May an update be fetched from this URL? https:// always; http:// only to 127.0.0.1 / localhost / [::1]
 * and only when allow_loopback_http (test builds). 1 = allowed. */
int release_url_allowed(const wchar_t* url, int allow_loopback_http);

/* ref relative to base (the manifest's URL): absolute ref as is, else base's folder + ref. */
void release_url_resolve(const wchar_t* base, const char* ref, wchar_t* out, size_t cap);

/* ---- the game folder: <dir>\game (current), game.new (staging), game.old (previous) ------------------ */
/* A finished staging folder holds ".complete"; game\sly2-version.txt names the version in a folder. */

/* Empty <dir>\game.new for a build (an earlier, unfinished one is removed). 1 = ok. */
int stage_begin(const wchar_t* dir, wchar_t* staging, size_t cap);
/* Marks game.new complete (writes .complete with the version). 1 = ok. */
int stage_mark_complete(const wchar_t* dir, const char* version);
/* game -> game.old (the one before is removed), game.new -> game; if the second step fails the first is
 * undone. 1 = swapped; 0 = nothing changed (err = Windows error). */
int stage_commit(const wchar_t* dir, unsigned long* err);
/* After an interruption (power loss between the two renames, or a crash while staging): no game but a
 * complete game.new -> it becomes game; no game but game.old -> back; an incomplete game.new is removed.
 * Returns what it did: 0 nothing, 1 finished a commit, 2 restored the previous version, 3 removed staging. */
int stage_recover(const wchar_t* dir);
/* game <-> game.old (go back to the previous version, or forward again). 1 = ok, 0 = nothing changed. */
int stage_rollback(const wchar_t* dir, unsigned long* err);
/* The version in <dir>\<sub>\sly2-version.txt ("" if none). */
void stage_version(const wchar_t* dir, const wchar_t* sub, char* out, size_t cap);
/* Removes a folder tree (never follows junctions). 1 = gone. */
int stage_delete_tree(const wchar_t* path);

/* ---- the AI model (Look = Reimagined): Setup / the update bundle carry models\x4plus.bin ---------------- */
#define MODEL_FILE L"x4plus.bin"
/* src_dir = the unpacked sources' models folder (<dir>\builder\models): x4plus.bin + x4plus.bin.sha256 ("<64 hex>  name",
 * from the same verified zip). Copies x4plus.bin to <staging>\models\x4plus.bin and checks the copy's SHA-256 against
 * it before the folder is marked complete. 1 = placed and verified; 0 = these sources carry no model (nothing done);
 * -1 = error (why says what; nothing left in staging\models). */
int stage_place_model(const wchar_t* src_dir, const wchar_t* staging, char* why, size_t cap);
/* SHA-256 of a file (bcrypt). 1 = ok. */
int release_sha256_file(const wchar_t* path, uint8_t out[32]);
