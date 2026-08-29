This is a Flipper application for editing and writing COGES Mykey Dongles. I’m releasing this because idiots are selling similar apps for profit and violating the MIKAI license.

This release is for educational purposes only and comes with zero support. If you message me saying, "Please halp me compile this so I can steal coffee/candy," I will publicly dox you to your school, employer, and the vendor. This is non-negotiable. In that case, you aren't a "hacker" you're just a shitty thief, and you deserve the fallout. Furthermore, if you own a Flipper but can't compile an app with the full source code provided, you have no business using this, get the fuck out.

Redistribution: You are NOT allowed to SELL or PUBLISH this application without the source code and this README. Feel free to update it, but you MUST adhere to these license requirements.

Credit for the original encryption algorithm goes to the MIKAI team.

---

## Features

### Card operations
- **Read Card** — waits up to 30 s for a card to be placed on the reader (150 ms retries, cancellable with Back), reads all 128 blocks, 3 read retries, and shows a real per-case result (success / no card / unsupported card / read failed / NFC busy / aborted). After a successful read an automatic backup is saved to SD (see below) and the app jumps straight to **View Info**.
- **Write to Card** — shows only when the loaded card data has been modified (`is_modified`). Waits up to 15 s for the card (cancellable), runs on a worker thread so the UI stays responsive, and skips the read-only UID block.
- **UID mismatch protection** — before writing, the card on the reader is read and its UID compared with the loaded one. If they differ, a confirmation dialog is shown; nothing is written unless the user explicitly confirms.
- **Post-write verification** — after writing, the card is re-read and every written block is compared. The result popup shows `Verify: N/M blocks OK`, or an error listing the mismatch if the data did not stick.
- **Automatic backup** — every successful read saves a copy to `/ext/apps_data/cogs_mikai/backup/UID_YYYYMMDD_HHMMSS.myk` (same format as Save to File, restorable via Load from File).

### Credit management
- **Add Credit** — preset buttons 1 / 2 / 5 / 10 / 20 EUR plus a custom amount entry (0.01–999.99 EUR). After adding, the write scene opens automatically (Back cancels the write; the credit stays in memory).
- **Set Credit** — preset buttons 5 / 10 / 20 / 50 EUR plus a custom amount entry; sets the credit to the exact value and opens the write scene automatically.
- **Reset Card** — resets the card data in memory; asks whether to write to the card right away.

### View Info
Scrollable card information: serial, vendor, current credit, status, operation count, UID and full transaction history (newest first). With a card loaded three buttons are available:
- **Menu** (left key) — back to the main menu, card stays loaded
- **Set Credit** (OK key) — open Set Credit directly
- **Add Credit** (right key) — open Add Credit directly

### Files
- **Save to File** — saves the loaded card to `/ext/apps_data/cogs_mikai/*.myk` (text format: UID, encryption key, all blocks).
- **Load from File** — restores a card from a `.myk` file.
- **Export History (CSV)** — exports card identity + transaction history to `/ext/apps_data/cogs_mikai/history_UID_YYYYMMDD_HHMMSS.csv`, readable in Excel/Calc.

### UI behavior
- Success confirmations auto-close after 1 s; error messages stay 2 s; the "Writing..." progress popup stays open until finished (cancellable with Back).
- The main menu is rebuilt on every entry: the header shows `[Card Loaded]` and ">>> Write to Card <<<" appears only when there are unmodified changes to write.

### Debug & About
- **Debug Info** — low-level card state dump.
- **About** — app credits.

## Build

```sh
./fbt fap_cogs_mikai
```

Prebuilt artifacts for API 88.2 are committed in `dist/`:

- `dist/cogs_mikai.fap` — FAP to copy to `/ext/apps` on the SD card
- `dist/debug/cogs_mikai_d.elf` — debug ELF for GDB (`arm-none-eabi-gdb`)

## Card data format

`.myk` files use the `COGES_MYKEY_V1` text format:

```
COGES_MYKEY_V1
UID: <16 hex>
ENCRYPTION_KEY: <8 hex>
BLOCK_000: <8 hex>
...
BLOCK_127: <8 hex>
```

## License

Original encryption algorithm credit goes to the MIKAI team. Redistribution is allowed only with the source code and this README; selling or publishing the app without source is prohibited.
