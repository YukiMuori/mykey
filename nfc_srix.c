#include "cogs_mikai.h"
#include <furi.h>
#include <machine/endian.h>
#include <furi_hal_nfc.h>
#include <nfc/nfc.h>
#include <nfc/protocols/st25tb/st25tb.h>
#include <nfc/protocols/st25tb/st25tb_poller_sync.h>

// How long to keep waiting for the card to be placed on the reader
#define MYKEY_READ_DETECT_TIMEOUT_MS (30000U)
// How long to keep waiting for the card before writing
#define MYKEY_WRITE_DETECT_TIMEOUT_MS (15000U)
// Delay between detection attempts while waiting for the card
#define MYKEY_READ_DETECT_RETRY_DELAY_MS (150U)
// Number of full card read attempts before giving up
#define MYKEY_READ_MAX_ATTEMPTS (3U)

MyKeyReadResult mykey_read_from_nfc(COGSMyKaiApp* app) {
    FURI_LOG_I(TAG, "Reading SRIX4K from NFC...");

    // nfc_alloc() would crash the app (furi_check) if the NFC hardware is
    // busy, so check availability first and report a clean error instead.
    if(furi_hal_nfc_acquire() != FuriHalNfcErrorNone) {
        FURI_LOG_E(TAG, "NFC hardware is busy");
        return MyKeyReadResultNfcBusy;
    }
    furi_hal_nfc_release();

    Nfc* nfc = nfc_alloc();

    // Phase 1: wait for a card to be placed on the reader.
    // st25tb_poller_sync_detect_type() performs a single detection attempt, so
    // retry until the card shows up, the user cancels or the timeout expires.
    St25tbType type;
    St25tbError error = St25tbErrorNotPresent;
    uint32_t start_tick = furi_get_tick();

    while(furi_get_tick() - start_tick < furi_ms_to_ticks(MYKEY_READ_DETECT_TIMEOUT_MS)) {
        if(app->op_abort) {
            FURI_LOG_I(TAG, "Read aborted by user");
            nfc_free(nfc);
            return MyKeyReadResultAborted;
        }

        error = st25tb_poller_sync_detect_type(nfc, &type);
        if(error == St25tbErrorNone) {
            break;
        }

        FURI_LOG_D(TAG, "Card not detected yet (error: %d), retrying...", error);
        furi_delay_ms(MYKEY_READ_DETECT_RETRY_DELAY_MS);
    }

    if(error != St25tbErrorNone) {
        FURI_LOG_E(
            TAG,
            "No ST25TB card detected within %u ms",
            MYKEY_READ_DETECT_TIMEOUT_MS);
        nfc_free(nfc);
        return app->op_abort ? MyKeyReadResultAborted : MyKeyReadResultNoCard;
    }

    // Check if it's SRIX4K (ST25TBX512 or ST25TB04K or ST25TBX4K)
    if(type != St25tbTypeX512 && type != St25tbType04k && type != St25tbTypeX4k) {
        FURI_LOG_E(TAG, "Card is not SRIX4K compatible, type: %d", type);
        nfc_free(nfc);
        return MyKeyReadResultUnsupportedCard;
    }

    FURI_LOG_I(TAG, "Detected ST25TB card type: %d", type);

    // Phase 2: read the entire card.
    // Retry a few times in case the card is removed or the link is flaky.
    St25tbData* st25tb_data = st25tb_alloc();
    bool read_ok = false;

    for(uint8_t attempt = 0; attempt < MYKEY_READ_MAX_ATTEMPTS && !read_ok; attempt++) {
        if(app->op_abort) {
            break;
        }

        error = st25tb_poller_sync_read(nfc, st25tb_data);
        if(error == St25tbErrorNone) {
            read_ok = true;
        } else {
            FURI_LOG_W(TAG, "Read attempt %u failed: %d", attempt + 1, error);
            furi_delay_ms(100);
        }
    }

    if(!read_ok) {
        FURI_LOG_E(TAG, "Failed to read ST25TB card");
        st25tb_free(st25tb_data);
        nfc_free(nfc);
        return app->op_abort ? MyKeyReadResultAborted : MyKeyReadResultReadFailed;
    }

    // Extract UID (8 bytes for ST25TB)
    // ST25TB UID bytes are in order [0..7], we need to assemble them big-endian
    // to match libmikai: uid[0] is MSB (bits 56-63), uid[7] is LSB (bits 0-7)
    app->mykey.uid = 0;
    for(size_t i = 0; i < ST25TB_UID_SIZE && i < 8; i++) {
        app->mykey.uid |= ((uint64_t)st25tb_data->uid[i]) << ((7 - i) * 8);
    }

    FURI_LOG_I(TAG, "Card UID (big-endian): %016llX", app->mykey.uid);
    FURI_LOG_I(TAG, "UID bytes: %02X %02X %02X %02X %02X %02X %02X %02X",
        st25tb_data->uid[0], st25tb_data->uid[1], st25tb_data->uid[2], st25tb_data->uid[3],
        st25tb_data->uid[4], st25tb_data->uid[5], st25tb_data->uid[6], st25tb_data->uid[7]);

    // Copy blocks to MyKey data structure
    // ST25TB stores data in blocks, we need to read all 128 blocks (512 bytes total)
    size_t num_blocks = st25tb_get_block_count(type);
    if(num_blocks > SRIX4K_BLOCKS) {
        num_blocks = SRIX4K_BLOCKS;
    }

    // ST25TB blocks need byte-swapping to match libmikai's big-endian format
    // Flipper SDK stores blocks in little-endian, libmikai expects big-endian
    for(size_t i = 0; i < num_blocks; i++) {
        app->mykey.eeprom[i] = __bswap32(st25tb_data->blocks[i]);
    }

    FURI_LOG_I(TAG, "Blocks byte-swapped to big-endian format");

    // Calculate encryption key from UID
    mykey_calculate_encryption_key(&app->mykey);

    // Update cached values
    app->mykey.is_loaded = true;
    app->mykey.is_modified = false;  // Fresh read from card
    app->mykey.is_reset = mykey_is_reset(&app->mykey);
    app->mykey.current_credit = mykey_get_current_credit(&app->mykey);

    FURI_LOG_I(TAG, "Card loaded successfully. Credit: %d cents, Reset: %s",
               app->mykey.current_credit,
               app->mykey.is_reset ? "Yes" : "No");

    st25tb_free(st25tb_data);
    nfc_free(nfc);

    return MyKeyReadResultOk;
}

bool mykey_write_to_nfc(COGSMyKaiApp* app) {
    FURI_LOG_I(TAG, "Writing to SRIX4K via NFC...");

    if(!app->mykey.is_loaded) {
        FURI_LOG_E(TAG, "No card data loaded, cannot write");
        return false;
    }

    // nfc_alloc() would crash the app (furi_check) if the NFC hardware is
    // busy, so check availability first and report a clean error instead.
    if(furi_hal_nfc_acquire() != FuriHalNfcErrorNone) {
        FURI_LOG_E(TAG, "NFC hardware is busy");
        return false;
    }
    furi_hal_nfc_release();

    Nfc* nfc = nfc_alloc();

    // Wait for the card to be placed on the reader (same retry strategy
    // as the read path). Abort-aware so Back cancels the wait.
    St25tbType type;
    St25tbError error = St25tbErrorNotPresent;
    uint32_t start_tick = furi_get_tick();

    while(furi_get_tick() - start_tick < furi_ms_to_ticks(MYKEY_WRITE_DETECT_TIMEOUT_MS)) {
        if(app->op_abort) {
            FURI_LOG_I(TAG, "Write aborted by user");
            nfc_free(nfc);
            return false;
        }

        error = st25tb_poller_sync_detect_type(nfc, &type);
        if(error == St25tbErrorNone) {
            break;
        }

        FURI_LOG_D(TAG, "Card not detected yet (error: %d), retrying...", error);
        furi_delay_ms(MYKEY_READ_DETECT_RETRY_DELAY_MS);
    }

    if(error != St25tbErrorNone) {
        FURI_LOG_E(TAG, "No ST25TB card detected within %u ms", MYKEY_WRITE_DETECT_TIMEOUT_MS);
        nfc_free(nfc);
        return false;
    }

    // Check if it's SRIX4K
    if(type != St25tbTypeX512 && type != St25tbType04k && type != St25tbTypeX4k) {
        FURI_LOG_E(TAG, "Card is not SRIX4K compatible, type: %d", type);
        nfc_free(nfc);
        return false;
    }

    size_t num_blocks = st25tb_get_block_count(type);
    if(num_blocks > SRIX4K_BLOCKS) {
        num_blocks = SRIX4K_BLOCKS;
    }

    // Write each block
    // Note: Block 0 (UID) is typically read-only, so we skip it
    bool success = true;
    for(size_t i = 1; i < num_blocks; i++) {
        if(app->op_abort) {
            success = false;
            break;
        }

        // Byte-swap block back to little-endian for ST25TB card
        // Our internal format is big-endian, ST25TB expects little-endian
        uint32_t block_to_write = __bswap32(app->mykey.eeprom[i]);
        error = st25tb_poller_sync_write_block(nfc, i, block_to_write);

        if(error != St25tbErrorNone) {
            FURI_LOG_E(TAG, "Failed to write block %zu: %d", i, error);
            success = false;
            // Continue trying to write remaining blocks
        }
    }

    if(success) {
        FURI_LOG_I(TAG, "Card written successfully");
    } else {
        FURI_LOG_W(TAG, "Card write completed with errors");
    }

    nfc_free(nfc);

    return success;
}
